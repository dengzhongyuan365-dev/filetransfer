#include "lan/transfer/single_file.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "lan/common/path.h"
#include "lan/common/stopwatch.h"
#include "lan/fs/file_descriptor.h"
#include "lan/fs/file_hash.h"
#include "lan/net/connection.h"
#include "lan/protocol/frame.h"
#include "lan/protocol/hello.h"
#include "lan/transfer/chunk_codec.h"
#include "lan/transfer/file_metadata.h"
#include "lan/transfer/part_file_guard.h"

namespace lan {

namespace {

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

Result<FileDescriptor> open_for_append(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
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

Result<bool> send_text_frame(Connection& connection, MessageType type, std::string_view message) {
    Frame frame;
    frame.type = type;
    frame.body = bytes_from_string(message);
    return write_frame(connection, frame);
}

Result<bool> send_ack_frame(Connection& connection, std::string_view message) {
    return send_text_frame(connection, MessageType::ack, message);
}

Result<bool> send_error_frame(Connection& connection, std::string_view message) {
    return send_text_frame(connection, MessageType::error, message);
}

Result<bool> wait_for_ack(Connection& connection, std::string_view context) {
    auto frame = read_frame(connection);
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

Result<FileBeginAckMetadata> wait_for_file_begin_ack(Connection& connection) {
    auto frame = read_frame(connection);
    if (!frame) {
        return Result<FileBeginAckMetadata>::failure(frame.error());
    }

    if (frame.value().type == MessageType::error) {
        return Result<FileBeginAckMetadata>::failure(
            make_error(ErrorCode::protocol_error,
                       "file_begin: receiver error: " + body_as_string(frame.value())));
    }

    if (frame.value().type != MessageType::ack) {
        return Result<FileBeginAckMetadata>::failure(
            make_error(ErrorCode::protocol_error,
                       "file_begin: expected ack or error frame, got " +
                           message_type_name(frame.value().type)));
    }

    return decode_file_begin_ack(body_as_string(frame.value()));
}

Result<ReceiveFileReport> fail_receive_with_error(Connection& connection, Error error) {
    (void)send_error_frame(connection, error.message);
    return Result<ReceiveFileReport>::failure(std::move(error));
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
    const auto status = std::filesystem::status(target, ec);
    if (status.type() == std::filesystem::file_type::directory) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::invalid_argument, "target path is a directory: " + quote_path(target)));
    }

    if (ec && status.type() != std::filesystem::file_type::not_found) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::io_error, "failed to inspect target " + quote_path(target) + ": " + ec.message()));
    }

    return Result<std::filesystem::path>::success(std::move(target));
}

Result<bool> existing_target_matches(const std::filesystem::path& target,
                                     const FileBeginMetadata& metadata) {
    std::error_code ec;
    const auto status = std::filesystem::status(target, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        return Result<bool>::success(false);
    }
    if (ec) {
        return Result<bool>::failure(
            make_error(ErrorCode::io_error,
                       "failed to inspect target " + quote_path(target) + ": " + ec.message()));
    }
    if (!std::filesystem::exists(status)) {
        return Result<bool>::success(false);
    }

    const auto size = std::filesystem::file_size(target, ec);
    if (ec) {
        return Result<bool>::failure(
            make_error(ErrorCode::io_error,
                       "failed to read target size " + quote_path(target) + ": " + ec.message()));
    }
    if (size != metadata.size) {
        return Result<bool>::success(false);
    }

    auto hash = hash_file(target);
    if (!hash) {
        return Result<bool>::failure(hash.error());
    }

    return Result<bool>::success(hash.value().hex_digest == metadata.sha256);
}

Result<bool> require_target_writable(const ReceiverConfig& config,
                                     const std::filesystem::path& target) {
    std::error_code ec;
    const auto status = std::filesystem::status(target, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        return Result<bool>::success(true);
    }
    if (ec) {
        return Result<bool>::failure(
            make_error(ErrorCode::io_error, "failed to inspect target " + quote_path(target) + ": " + ec.message()));
    }
    if (std::filesystem::exists(status) && !config.allow_overwrite) {
        return Result<bool>::failure(
            make_error(ErrorCode::invalid_argument, "target already exists: " + quote_path(target)));
    }

    return Result<bool>::success(true);
}

Result<std::uint64_t> choose_resume_offset(const std::filesystem::path& part_path,
                                           const FileBeginMetadata& metadata) {
    if (!metadata.resume) {
        return Result<std::uint64_t>::success(0);
    }

    std::error_code ec;
    if (!std::filesystem::exists(part_path, ec)) {
        if (ec) {
            return Result<std::uint64_t>::failure(
                make_error(ErrorCode::io_error,
                           "failed to inspect part file " + quote_path(part_path) + ": " + ec.message()));
        }
        return Result<std::uint64_t>::success(0);
    }

    const auto size = std::filesystem::file_size(part_path, ec);
    if (ec) {
        return Result<std::uint64_t>::failure(
            make_error(ErrorCode::io_error,
                       "failed to read part file size " + quote_path(part_path) + ": " + ec.message()));
    }

    if (size >= metadata.size) {
        return Result<std::uint64_t>::success(0);
    }

    return Result<std::uint64_t>::success(size);
}

}  // namespace

Result<SendFileReport> send_single_file(const SenderConfig& config) {
    CancellationToken cancellation;
    return send_single_file(config, SendFileProgressCallback{}, cancellation);
}

Result<SendFileReport> send_single_file(const SenderConfig& config,
                                        const CancellationToken& cancellation) {
    return send_single_file(config, SendFileProgressCallback{}, cancellation);
}

Result<SendFileReport> send_single_file(const SenderConfig& config,
                                        SendFileProgressCallback on_progress) {
    CancellationToken cancellation;
    return send_single_file(config, std::move(on_progress), cancellation);
}

Result<SendFileReport> send_single_file(const SenderConfig& config,
                                        SendFileProgressCallback on_progress,
                                        const CancellationToken& cancellation) {
    auto connection = default_network_backend().connect(config.target.host, config.target.port);
    if (!connection) {
        return Result<SendFileReport>::failure(connection.error());
    }

    return send_single_file_to_connection(config, *connection.value(), std::move(on_progress), cancellation);
}

Result<SendFileReport> send_single_file_to_connection(const SenderConfig& config,
                                                       Connection& connection) {
    CancellationToken cancellation;
    return send_single_file_to_connection(config, connection, SendFileProgressCallback{}, cancellation);
}

Result<SendFileReport> send_single_file_to_connection(const SenderConfig& config,
                                                       Connection& connection,
                                                       const CancellationToken& cancellation) {
    return send_single_file_to_connection(config, connection, SendFileProgressCallback{}, cancellation);
}

Result<SendFileReport> send_single_file_to_connection(
    const SenderConfig& config,
    Connection& connection,
    SendFileProgressCallback on_progress) {
    CancellationToken cancellation;
    return send_single_file_to_connection(config, connection, std::move(on_progress), cancellation);
}

Result<SendFileReport> send_single_file_to_connection(
    const SenderConfig& config,
    Connection& connection,
    SendFileProgressCallback on_progress,
    const CancellationToken& cancellation) {
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

    const auto file_name = config.source_path.filename().string();

    Frame hello;
    hello.type = MessageType::hello;
    hello.body = encode_hello(HelloMetadata{.mode = HelloMode::file, .sender_id = config.sender_id});
    auto hello_result = write_frame(connection, hello);
    if (!hello_result) {
        return Result<SendFileReport>::failure(hello_result.error());
    }

    Frame begin;
    begin.type = MessageType::file_begin;
    begin.body = bytes_from_string(encode_file_begin(FileBeginMetadata{
        .name = file_name,
        .size = size.value(),
        .sha256 = hash.value().hex_digest,
        .resume = config.resume,
        .source = config.source,
    }));
    auto begin_result = write_frame(connection, begin);
    if (!begin_result) {
        return Result<SendFileReport>::failure(begin_result.error());
    }

    auto begin_ack = wait_for_file_begin_ack(connection);
    if (!begin_ack) {
        return Result<SendFileReport>::failure(begin_ack.error());
    }
    if (begin_ack.value().offset > size.value()) {
        return Result<SendFileReport>::failure(
            make_error(ErrorCode::protocol_error, "receiver requested offset beyond source file size"));
    }
    if (begin_ack.value().complete && begin_ack.value().offset != size.value()) {
        return Result<SendFileReport>::failure(
            make_error(ErrorCode::protocol_error, "receiver marked file complete at wrong offset"));
    }
    const auto status = begin_ack.value().complete
                            ? FileTransferStatus::skipped
                            : (begin_ack.value().offset > 0 ? FileTransferStatus::resumed
                                                            : FileTransferStatus::transferred);

    auto source = open_for_read(config.source_path);
    if (!source) {
        return Result<SendFileReport>::failure(source.error());
    }
    if (begin_ack.value().offset > 0) {
        const auto seek_result = ::lseek(
            source.value().get(), static_cast<off_t>(begin_ack.value().offset), SEEK_SET);
        if (seek_result < 0) {
            return Result<SendFileReport>::failure(
                make_error(ErrorCode::io_error, errno_message("failed to seek", config.source_path, errno)));
        }
    }

    Stopwatch transfer_timer;
    std::vector<std::byte> buffer(static_cast<std::size_t>(config.chunk_size));
    std::uint64_t bytes_sent = begin_ack.value().offset;
    auto publish_progress = [&] {
        if (on_progress) {
            on_progress(SendFileProgress{
                .source_path = config.source_path,
                .file_name = file_name,
                .bytes_sent = bytes_sent,
                .total_bytes = size.value(),
                .elapsed_seconds = transfer_timer.elapsed_seconds(),
            });
        }
    };
    publish_progress();

    while (!begin_ack.value().complete) {
        if (cancellation.is_cancelled()) {
            return Result<SendFileReport>::failure(
                make_error(ErrorCode::cancelled, "send file transfer cancelled"));
        }

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
        chunk.body = encode_chunk_body(
            bytes_sent, buffer.data(), static_cast<std::size_t>(bytes_read));

        auto chunk_result = write_frame(connection, chunk);
        if (!chunk_result) {
            return Result<SendFileReport>::failure(chunk_result.error());
        }

        bytes_sent += static_cast<std::uint64_t>(bytes_read);
        publish_progress();
    }

    Frame end;
    end.type = MessageType::file_end;
    auto end_result = write_frame(connection, end);
    if (!end_result) {
        return Result<SendFileReport>::failure(end_result.error());
    }

    auto end_ack = wait_for_ack(connection, "file_end");
    if (!end_ack) {
        return Result<SendFileReport>::failure(end_ack.error());
    }

    return Result<SendFileReport>::success(SendFileReport{
        .file_name = file_name,
        .bytes_sent = bytes_sent,
        .sha256 = hash.value().hex_digest,
        .elapsed_seconds = transfer_timer.elapsed_seconds(),
        .status = status,
        .resumed_from = begin_ack.value().offset,
        .source = config.source,
    });
}

Result<ReceiveFileReport> receive_single_file(const ReceiverConfig& config) {
    auto listener = default_network_backend().listen(config.bind_address, config.port);
    if (!listener) {
        return Result<ReceiveFileReport>::failure(listener.error());
    }

    auto client = listener.value()->accept();
    if (!client) {
        return Result<ReceiveFileReport>::failure(client.error());
    }

    auto hello = read_frame(*client.value());
    if (!hello) {
        return Result<ReceiveFileReport>::failure(hello.error());
    }
    auto metadata = decode_hello_frame(hello.value());
    if (!metadata) {
        return Result<ReceiveFileReport>::failure(metadata.error());
    }
    if (metadata.value().mode != HelloMode::file) {
        return Result<ReceiveFileReport>::failure(
            make_error(ErrorCode::protocol_error, "expected file hello frame"));
    }

    return receive_single_file_from_connection(config, *client.value(), hello.value());
}

Result<ReceiveFileReport> receive_single_file_from_connection(const ReceiverConfig& config,
                                                              Connection& connection,
                                                              const Frame& hello) {
    return receive_single_file_from_connection(config, connection, hello, {});
}

Result<ReceiveFileReport> receive_single_file_from_connection(
    const ReceiverConfig& config,
    Connection& connection,
    const Frame& hello,
    ReceiveFileProgressCallback on_progress) {
    auto hello_metadata = decode_hello_frame(hello);
    if (!hello_metadata) {
        return Result<ReceiveFileReport>::failure(hello_metadata.error());
    }
    if (hello_metadata.value().mode != HelloMode::file) {
        return Result<ReceiveFileReport>::failure(
            make_error(ErrorCode::protocol_error, "expected file hello frame"));
    }

    auto begin = read_frame(connection);
    if (!begin) {
        return Result<ReceiveFileReport>::failure(begin.error());
    }
    if (begin.value().type != MessageType::file_begin) {
        return Result<ReceiveFileReport>::failure(
            make_error(ErrorCode::protocol_error, "expected file_begin frame"));
    }

    auto metadata = decode_file_begin(body_as_string(begin.value()));
    if (!metadata) {
        return fail_receive_with_error(connection, metadata.error());
    }

    auto ready_config = config;
    auto receive_dir = ensure_directory(config.receive_dir);
    if (!receive_dir) {
        return fail_receive_with_error(connection, receive_dir.error());
    }
    ready_config.receive_dir = std::move(receive_dir).value();

    auto target = safe_target_path(ready_config, metadata.value().name);
    if (!target) {
        return fail_receive_with_error(connection, target.error());
    }

    auto matched = existing_target_matches(target.value(), metadata.value());
    if (!matched) {
        return fail_receive_with_error(connection, matched.error());
    }
    if (matched.value()) {
        auto begin_ack = send_ack_frame(
            connection,
            encode_file_begin_ack(FileBeginAckMetadata{
                .offset = metadata.value().size,
                .complete = true,
            }));
        if (!begin_ack) {
            return Result<ReceiveFileReport>::failure(begin_ack.error());
        }

        auto end = read_frame(connection);
        if (!end) {
            return Result<ReceiveFileReport>::failure(end.error());
        }
        if (end.value().type != MessageType::file_end) {
            return fail_receive_with_error(
                connection,
                make_error(ErrorCode::protocol_error, "expected file_end frame"));
        }

        auto end_ack = send_ack_frame(connection, "stored");
        if (!end_ack) {
            return Result<ReceiveFileReport>::failure(end_ack.error());
        }

        return Result<ReceiveFileReport>::success(ReceiveFileReport{
            .target_path = target.value(),
            .bytes_received = metadata.value().size,
            .sha256 = metadata.value().sha256,
            .elapsed_seconds = 0.0,
            .status = FileTransferStatus::skipped,
            .source = metadata.value().source,
        });
    }

    auto writable = require_target_writable(config, target.value());
    if (!writable) {
        return fail_receive_with_error(connection, writable.error());
    }

    const auto part_path = part_path_for(target.value());
    auto resume_offset = choose_resume_offset(part_path, metadata.value());
    if (!resume_offset) {
        return fail_receive_with_error(connection, resume_offset.error());
    }

    auto output = resume_offset.value() == 0 ? open_for_write(part_path)
                                             : open_for_append(part_path);
    if (!output) {
        return fail_receive_with_error(connection, output.error());
    }
    PartFileGuard part_file(part_path);

    auto begin_ack = send_ack_frame(
        connection, encode_file_begin_ack(FileBeginAckMetadata{.offset = resume_offset.value()}));
    if (!begin_ack) {
        return Result<ReceiveFileReport>::failure(begin_ack.error());
    }

    Stopwatch transfer_timer;
    std::uint64_t bytes_received = resume_offset.value();
    const auto status = resume_offset.value() > 0 ? FileTransferStatus::resumed
                                                  : FileTransferStatus::transferred;
    auto publish_progress = [&] {
        if (on_progress) {
            on_progress(ReceiveFileProgress{
                .target_path = target.value(),
                .file_name = metadata.value().name,
                .bytes_received = bytes_received,
                .total_bytes = metadata.value().size,
                .elapsed_seconds = transfer_timer.elapsed_seconds(),
            });
        }
    };
    publish_progress();

    while (true) {
        auto frame = read_frame(connection);
        if (!frame) {
            return Result<ReceiveFileReport>::failure(frame.error());
        }

        if (frame.value().type == MessageType::file_end) {
            break;
        }

        if (frame.value().type != MessageType::chunk) {
            return fail_receive_with_error(
                connection,
                make_error(ErrorCode::protocol_error, "expected chunk or file_end frame"));
        }

        auto chunk = decode_chunk_body(frame.value());
        if (!chunk) {
            return fail_receive_with_error(connection, chunk.error());
        }

        if (chunk.value().offset != bytes_received) {
            return fail_receive_with_error(
                connection,
                make_error(ErrorCode::protocol_error,
                           "chunk offset does not match received byte count"));
        }

        if (chunk.value().size > metadata.value().size - bytes_received) {
            return fail_receive_with_error(
                connection,
                make_error(ErrorCode::protocol_error,
                           "chunk exceeds declared file size"));
        }

        auto write_result = write_all_file(output.value(), chunk.value().data, chunk.value().size);
        if (!write_result) {
            return fail_receive_with_error(connection, write_result.error());
        }

        bytes_received += static_cast<std::uint64_t>(chunk.value().size);
        publish_progress();
    }

    if (bytes_received != metadata.value().size) {
        return fail_receive_with_error(
            connection,
            make_error(ErrorCode::protocol_error,
                       "received byte count does not match file_begin size"));
    }

    if (::fsync(output.value().get()) != 0) {
        return fail_receive_with_error(
            connection,
            make_error(ErrorCode::io_error, errno_message("failed to fsync", part_path, errno)));
    }

    output.value().reset();

    auto received_hash = hash_file(part_path);
    if (!received_hash) {
        return fail_receive_with_error(connection, received_hash.error());
    }

    if (received_hash.value().hex_digest != metadata.value().sha256) {
        return fail_receive_with_error(
            connection,
            make_error(ErrorCode::checksum_mismatch,
                       "received file sha256 does not match sender metadata"));
    }

    std::error_code ec;
    std::filesystem::rename(part_path, target.value(), ec);
    if (ec) {
        return fail_receive_with_error(
            connection,
            make_error(ErrorCode::io_error,
                       "failed to rename " + quote_path(part_path) + " to " +
                           quote_path(target.value()) + ": " + ec.message()));
    }
    part_file.commit();

    auto end_ack = send_ack_frame(connection, "stored");
    if (!end_ack) {
        return Result<ReceiveFileReport>::failure(end_ack.error());
    }

    return Result<ReceiveFileReport>::success(ReceiveFileReport{
        .target_path = target.value(),
        .bytes_received = bytes_received,
        .sha256 = received_hash.value().hex_digest,
        .elapsed_seconds = transfer_timer.elapsed_seconds(),
        .status = status,
        .resumed_from = resume_offset.value(),
        .source = metadata.value().source,
    });
}

}  // namespace lan
