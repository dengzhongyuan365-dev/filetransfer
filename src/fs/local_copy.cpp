#include "lan/fs/local_copy.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "lan/common/stopwatch.h"
#include "lan/fs/file_descriptor.h"
#include "lan/fs/file_hash.h"

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

Result<std::uint64_t> read_file_size(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return Result<std::uint64_t>::failure(
            make_error(ErrorCode::io_error, "failed to read file size " + quote_path(path) + ": " + ec.message()));
    }
    return Result<std::uint64_t>::success(size);
}

Result<std::filesystem::path> validate_source_file(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            return Result<std::filesystem::path>::failure(
                make_error(ErrorCode::not_found, "source file does not exist: " + quote_path(path)));
        }

        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::io_error, "failed to inspect source " + quote_path(path) + ": " + ec.message()));
    }

    if (!std::filesystem::is_regular_file(status)) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::invalid_argument, "source must be a regular file: " + quote_path(path)));
    }

    return Result<std::filesystem::path>::success(path);
}

Result<std::filesystem::path> make_part_path(const std::filesystem::path& target) {
    if (target.empty()) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::invalid_argument, "target path must not be empty"));
    }

    auto part = target;
    part += ".part";
    return Result<std::filesystem::path>::success(part);
}

Result<bool> ensure_target_is_available(const std::filesystem::path& target, bool overwrite) {
    std::error_code ec;
    if (std::filesystem::exists(target, ec) && !overwrite) {
        return Result<bool>::failure(
            make_error(ErrorCode::invalid_argument, "target already exists: " + quote_path(target)));
    }

    if (ec) {
        return Result<bool>::failure(
            make_error(ErrorCode::io_error, "failed to inspect target " + quote_path(target) + ": " + ec.message()));
    }

    const auto parent = target.parent_path();
    if (!parent.empty() && !std::filesystem::is_directory(parent, ec)) {
        if (ec) {
            return Result<bool>::failure(
                make_error(ErrorCode::io_error,
                           "failed to inspect target parent " + quote_path(parent) + ": " + ec.message()));
        }

        return Result<bool>::failure(
            make_error(ErrorCode::not_found, "target parent directory does not exist: " + quote_path(parent)));
    }

    return Result<bool>::success(true);
}

Result<bool> write_all(int fd, const char* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const auto result = ::write(fd, data + written, size - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return Result<bool>::failure(
                make_error(ErrorCode::io_error, "failed to write target file: " + std::string(std::strerror(errno))));
        }

        written += static_cast<std::size_t>(result);
    }

    return Result<bool>::success(true);
}

}  // namespace

Result<LocalCopyReport> copy_file(const LocalCopyOptions& options) {
    if (options.buffer_size == 0) {
        return Result<LocalCopyReport>::failure(
            make_error(ErrorCode::invalid_argument, "buffer size must be greater than zero"));
    }

    auto source_valid = validate_source_file(options.source_path);
    if (!source_valid) {
        return Result<LocalCopyReport>::failure(source_valid.error());
    }

    auto target_available = ensure_target_is_available(options.target_path, options.overwrite);
    if (!target_available) {
        return Result<LocalCopyReport>::failure(target_available.error());
    }

    auto part_path = make_part_path(options.target_path);
    if (!part_path) {
        return Result<LocalCopyReport>::failure(part_path.error());
    }

    auto total_size = read_file_size(options.source_path);
    if (!total_size) {
        return Result<LocalCopyReport>::failure(total_size.error());
    }

    auto source = open_for_read(options.source_path);
    if (!source) {
        return Result<LocalCopyReport>::failure(source.error());
    }

    auto target = open_for_write(part_path.value());
    if (!target) {
        return Result<LocalCopyReport>::failure(target.error());
    }

    std::vector<char> buffer(static_cast<std::size_t>(options.buffer_size));
    std::uint64_t bytes_copied = 0;
    Stopwatch stopwatch;

    while (true) {
        const auto bytes_read = ::read(source.value().get(), buffer.data(), buffer.size());
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }

            return Result<LocalCopyReport>::failure(
                make_error(ErrorCode::io_error,
                           errno_message("failed to read", options.source_path, errno)));
        }

        if (bytes_read == 0) {
            break;
        }

        auto write_result = write_all(target.value().get(), buffer.data(), static_cast<std::size_t>(bytes_read));
        if (!write_result) {
            return Result<LocalCopyReport>::failure(write_result.error());
        }

        bytes_copied += static_cast<std::uint64_t>(bytes_read);
        if (options.on_progress) {
            options.on_progress(LocalCopyProgress{
                .bytes_copied = bytes_copied,
                .total_bytes = total_size.value(),
                .elapsed_seconds = stopwatch.elapsed_seconds(),
            });
        }
    }

    if (::fsync(target.value().get()) != 0) {
        return Result<LocalCopyReport>::failure(
            make_error(ErrorCode::io_error, errno_message("failed to fsync", part_path.value(), errno)));
    }

    target.value().reset();

    std::error_code ec;
    std::filesystem::rename(part_path.value(), options.target_path, ec);
    if (ec) {
        return Result<LocalCopyReport>::failure(
            make_error(ErrorCode::io_error,
                       "failed to rename " + quote_path(part_path.value()) + " to " +
                           quote_path(options.target_path) + ": " + ec.message()));
    }

    auto source_hash = hash_file(options.source_path, options.buffer_size);
    if (!source_hash) {
        return Result<LocalCopyReport>::failure(source_hash.error());
    }

    auto target_hash = hash_file(options.target_path, options.buffer_size);
    if (!target_hash) {
        return Result<LocalCopyReport>::failure(target_hash.error());
    }

    if (source_hash.value().hex_digest != target_hash.value().hex_digest) {
        return Result<LocalCopyReport>::failure(
            make_error(ErrorCode::checksum_mismatch,
                       "copy verification failed: source and target sha256 differ"));
    }

    return Result<LocalCopyReport>::success(LocalCopyReport{
        .bytes_copied = bytes_copied,
        .elapsed_seconds = stopwatch.elapsed_seconds(),
        .source_hash = std::move(source_hash).value(),
        .target_hash = std::move(target_hash).value(),
    });
}

}  // namespace lan
