#include "lan/transfer/single_file.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "lan/common/parse.h"
#include "lan/fs/file_descriptor.h"
#include "lan/fs/file_hash.h"
#include "lan/net/tcp.h"
#include "lan/protocol/frame.h"

namespace lan {

namespace {

struct FileBeginMetadata {
    std::string name;
    std::uint64_t size = 0;
    std::string sha256;
};

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

std::string quote_path(const std::filesystem::path& path) {
    return "'" + path.string() + "'";
}

std::string errno_message(const std::string& action, const std::filesystem::path& path, int error) {
    return action + " " + quote_path(path) + ": " + std::strerror(error);
}

Result<FileDescriptor> open_for_read(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Result<FileDescriptor>::failure(
            make_error(ErrorCode::io_error, errno_message("failed to open", path, errno)));
    }

    return Result<FileDescriptor>::success(FileDescriptor(fd));
}

Result<FileDescriptor> open_for_write(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        return Result<FileDescriptor>::failure(
            make_error(ErrorCode::io_error, errno_message("failed to open", path, errno)));
    }

    return Result<FileDescriptor>::success(FileDescriptor(fd));
}

Result<bool> write_all_file(const FileDescriptor& file, const std::byte* data, std::size_t size) {
    std::size_t written = 0;
    const auto* bytes = reinterpret_cast<const char*>(data);

    while (written < size) {
        const auto result = ::write(file.get(), bytes + written, size - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return Result<bool>::failure(
                make_error(ErrorCode::io_error, "failed to write received file: " + std::string(std::strerror(errno))));
        }

        if (result == 0) {
            return Result<bool>::failure(
                make_error(ErrorCode::io_error, "failed to write received file: write returned zero"));
        }

        written += static_cast<std::size_t>(result);
    }

    return Result<bool>::success(true);
}

Result<std::uint64_t> read_file_size(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return Result<std::uint64_t>::failure(
            make_error(ErrorCode::io_error, "failed to read file size " + quote_path(path) + ": " + ec.message()));
    }

    return Result<std::uint64_t>::success(size);
}

Result<bool> require_regular_file(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec) {
        return Result<bool>::failure(
            make_error(ErrorCode::io_error, "failed to inspect source " + quote_path(path) + ": " + ec.message()));
    }

    if (!std::filesystem::is_regular_file(status)) {
        return Result<bool>::failure(
            make_error(ErrorCode::invalid_argument,
                       "single file transfer requires a regular file: " + quote_path(path)));
    }

    return Result<bool>::success(true);
}

Result<bool> send_text_frame(const FileDescriptor& socket, MessageType type, std::string_view message) {
    Frame frame;
    frame.type = type;
    frame.body = bytes_from_string(message);
    return write_frame(socket, frame);
}

Result<bool> send_ack_frame(const FileDescriptor& socket, std::string_view message) {
    return send_text_frame(socket, MessageType::ack, message);
}

Result<bool> send_error_frame(const FileDescriptor& socket, std::string_view message) {
    return send_text_frame(socket, MessageType::error, message);
}

Result<bool> wait_for_ack(const FileDescriptor& socket, std::string_view context) {
    auto frame = read_frame(socket);
    if (!frame) {
        return Result<bool>::failure(frame.error());
    }

    if (frame.value().type == MessageType::ack) {
        return Result<bool>::success(true);
    }

    if (frame.value().type == MessageType::error) {
        return Result<bool>::failure(
            make_error(ErrorCode::protocol_error,
                       std::string(context) + ": receiver error: " + body_as_string(frame.value())));
    }

    return Result<bool>::failure(
        make_error(ErrorCode::protocol_error,
                   std::string(context) + ": expected ack or error frame, got " +
                       message_type_name(frame.value().type)));
}

Result<ReceiveFileReport> fail_receive_with_error(const FileDescriptor& socket, Error error) {
    (void)send_error_frame(socket, error.message);
    return Result<ReceiveFileReport>::failure(std::move(error));
}

std::string encode_file_begin(const FileBeginMetadata& metadata) {
    return "name=" + metadata.name + "\n" +
           "size=" + std::to_string(metadata.size) + "\n" +
           "sha256=" + metadata.sha256 + "\n";
}

Result<FileBeginMetadata> decode_file_begin(std::string_view body) {
    FileBeginMetadata metadata;
    bool has_name = false;
    bool has_size = false;
    bool has_sha256 = false;

    std::size_t start = 0;
    while (start < body.size()) {
        const auto end = body.find('\n', start);
        const auto line = body.substr(start, end == std::string_view::npos ? body.size() - start : end - start);

        const auto separator = line.find('=');
        if (separator != std::string_view::npos) {
            const auto key = line.substr(0, separator);
            const auto value = line.substr(separator + 1);

            if (key == "name") {
                metadata.name = std::string(value);
                has_name = true;
            } else if (key == "size") {
                auto parsed = parse_size(value);
                if (!parsed) {
                    return Result<FileBeginMetadata>::failure(parsed.error());
                }
                metadata.size = parsed.value();
                has_size = true;
            } else if (key == "sha256") {
                metadata.sha256 = std::string(value);
                has_sha256 = true;
            }
        }

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    if (!has_name || metadata.name.empty()) {
        return Result<FileBeginMetadata>::failure(
            make_error(ErrorCode::protocol_error, "file_begin is missing name"));
    }
    if (!has_size) {
        return Result<FileBeginMetadata>::failure(
            make_error(ErrorCode::protocol_error, "file_begin is missing size"));
    }
    if (!has_sha256 || metadata.sha256.empty()) {
        return Result<FileBeginMetadata>::failure(
            make_error(ErrorCode::protocol_error, "file_begin is missing sha256"));
    }

    return Result<FileBeginMetadata>::success(std::move(metadata));
}

std::filesystem::path part_path_for(const std::filesystem::path& target) {
    auto part = target;
    part += ".part";
    return part;
}

Result<std::filesystem::path> safe_target_path(const ReceiverConfig& config, std::string_view remote_name) {
    const auto filename = std::filesystem::path(std::string(remote_name)).filename();
    if (filename.empty() || filename == "." || filename == "..") {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::protocol_error, "received invalid file name"));
    }

    auto target = config.receive_dir / filename;

    std::error_code ec;
    if (std::filesystem::is_directory(target, ec)) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::invalid_argument, "target path is a directory: " + quote_path(target)));
    }

    if (std::filesystem::exists(target, ec) && !config.allow_overwrite) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::invalid_argument, "target already exists: " + quote_path(target)));
    }

    if (ec) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::io_error, "failed to inspect target " + quote_path(target) + ": " + ec.message()));
    }

    return Result<std::filesystem::path>::success(std::move(target));
}

}  // namespace

Result<SendFileReport> send_single_file(const SenderConfig& config) {
    auto regular_file = require_regular_file(config.source_path);
    if (!regular_file) {
        return Result<SendFileReport>::failure(regular_file.error());
    }

    auto size = read_file_size(config.source_path);
    if (!size) {
        return Result<SendFileReport>::failure(size.error());
    }

    auto hash = hash_file(config.source_path, config.chunk_size);
    if (!hash) {
        return Result<SendFileReport>::failure(hash.error());
    }

    auto socket = connect_tcp(config.target.host, config.target.port);
    if (!socket) {
        return Result<SendFileReport>::failure(socket.error());
    }

    const auto file_name = config.source_path.filename().string();

    Frame hello;
    hello.type = MessageType::hello;
    hello.body = bytes_from_string(file_name);
    auto hello_result = write_frame(socket.value(), hello);
    if (!hello_result) {
        return Result<SendFileReport>::failure(hello_result.error());
    }

    Frame begin;
    begin.type = MessageType::file_begin;
    begin.body = bytes_from_string(encode_file_begin(FileBeginMetadata{
        .name = file_name,
        .size = size.value(),
        .sha256 = hash.value().hex_digest,
    }));
    auto begin_result = write_frame(socket.value(), begin);
    if (!begin_result) {
        return Result<SendFileReport>::failure(begin_result.error());
    }

    auto begin_ack = wait_for_ack(socket.value(), "file_begin");
    if (!begin_ack) {
        return Result<SendFileReport>::failure(begin_ack.error());
    }

    auto source = open_for_read(config.source_path);
    if (!source) {
        return Result<SendFileReport>::failure(source.error());
    }

    std::vector<std::byte> buffer(static_cast<std::size_t>(config.chunk_size));
    std::uint64_t bytes_sent = 0;

    while (true) {
        const auto bytes_read = ::read(source.value().get(), buffer.data(), buffer.size());
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }

            return Result<SendFileReport>::failure(
                make_error(ErrorCode::io_error, errno_message("failed to read", config.source_path, errno)));
        }

        if (bytes_read == 0) {
            break;
        }

        Frame chunk;
        chunk.type = MessageType::chunk;
        chunk.body.assign(buffer.begin(), buffer.begin() + bytes_read);

        auto chunk_result = write_frame(socket.value(), chunk);
        if (!chunk_result) {
            return Result<SendFileReport>::failure(chunk_result.error());
        }

        bytes_sent += static_cast<std::uint64_t>(bytes_read);
    }

    Frame end;
    end.type = MessageType::file_end;
    auto end_result = write_frame(socket.value(), end);
    if (!end_result) {
        return Result<SendFileReport>::failure(end_result.error());
    }

    auto end_ack = wait_for_ack(socket.value(), "file_end");
    if (!end_ack) {
        return Result<SendFileReport>::failure(end_ack.error());
    }

    return Result<SendFileReport>::success(SendFileReport{
        .file_name = file_name,
        .bytes_sent = bytes_sent,
        .sha256 = hash.value().hex_digest,
    });
}

Result<ReceiveFileReport> receive_single_file(const ReceiverConfig& config) {
    auto listener = listen_tcp(config.bind_address, config.port);
    if (!listener) {
        return Result<ReceiveFileReport>::failure(listener.error());
    }

    auto client = accept_tcp(listener.value());
    if (!client) {
        return Result<ReceiveFileReport>::failure(client.error());
    }

    auto hello = read_frame(client.value());
    if (!hello) {
        return Result<ReceiveFileReport>::failure(hello.error());
    }
    if (hello.value().type != MessageType::hello) {
        return Result<ReceiveFileReport>::failure(
            make_error(ErrorCode::protocol_error, "expected hello frame"));
    }

    auto begin = read_frame(client.value());
    if (!begin) {
        return Result<ReceiveFileReport>::failure(begin.error());
    }
    if (begin.value().type != MessageType::file_begin) {
        return Result<ReceiveFileReport>::failure(
            make_error(ErrorCode::protocol_error, "expected file_begin frame"));
    }

    auto metadata = decode_file_begin(body_as_string(begin.value()));
    if (!metadata) {
        return fail_receive_with_error(client.value(), metadata.error());
    }

    auto target = safe_target_path(config, metadata.value().name);
    if (!target) {
        return fail_receive_with_error(client.value(), target.error());
    }

    const auto part_path = part_path_for(target.value());
    auto output = open_for_write(part_path);
    if (!output) {
        return fail_receive_with_error(client.value(), output.error());
    }

    auto begin_ack = send_ack_frame(client.value(), "ready");
    if (!begin_ack) {
        return Result<ReceiveFileReport>::failure(begin_ack.error());
    }

    std::uint64_t bytes_received = 0;

    while (true) {
        auto frame = read_frame(client.value());
        if (!frame) {
            return Result<ReceiveFileReport>::failure(frame.error());
        }

        if (frame.value().type == MessageType::file_end) {
            break;
        }

        if (frame.value().type != MessageType::chunk) {
            return fail_receive_with_error(
                client.value(),
                make_error(ErrorCode::protocol_error, "expected chunk or file_end frame"));
        }

        auto write_result = write_all_file(output.value(), frame.value().body.data(), frame.value().body.size());
        if (!write_result) {
            return fail_receive_with_error(client.value(), write_result.error());
        }

        bytes_received += static_cast<std::uint64_t>(frame.value().body.size());
    }

    if (bytes_received != metadata.value().size) {
        return fail_receive_with_error(
            client.value(),
            make_error(ErrorCode::protocol_error,
                       "received byte count does not match file_begin size"));
    }

    if (::fsync(output.value().get()) != 0) {
        return fail_receive_with_error(
            client.value(),
            make_error(ErrorCode::io_error, errno_message("failed to fsync", part_path, errno)));
    }

    output.value().reset();

    auto received_hash = hash_file(part_path);
    if (!received_hash) {
        return fail_receive_with_error(client.value(), received_hash.error());
    }

    if (received_hash.value().hex_digest != metadata.value().sha256) {
        return fail_receive_with_error(
            client.value(),
            make_error(ErrorCode::checksum_mismatch,
                       "received file sha256 does not match sender metadata"));
    }

    std::error_code ec;
    std::filesystem::rename(part_path, target.value(), ec);
    if (ec) {
        return fail_receive_with_error(
            client.value(),
            make_error(ErrorCode::io_error,
                       "failed to rename " + quote_path(part_path) + " to " +
                           quote_path(target.value()) + ": " + ec.message()));
    }

    auto end_ack = send_ack_frame(client.value(), "stored");
    if (!end_ack) {
        return Result<ReceiveFileReport>::failure(end_ack.error());
    }

    return Result<ReceiveFileReport>::success(ReceiveFileReport{
        .target_path = target.value(),
        .bytes_received = bytes_received,
        .sha256 = received_hash.value().hex_digest,
    });
}

}  // namespace lan
