#include "lan/transfer/sync_executor.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "lan/fs/file_descriptor.h"
#include "lan/fs/file_hash.h"
#include "lan/transfer/delta.h"
#include "lan/transfer/part_file_guard.h"

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

std::string quote_path(const std::filesystem::path& path) {
    return "'" + path.string() + "'";
}

std::filesystem::path part_path_for(const std::filesystem::path& target) {
    auto part = target;
    part += ".part";
    return part;
}

Result<FileDescriptor> open_for_read(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Result<FileDescriptor>::failure(
            make_error(ErrorCode::io_error,
                       "failed to open " + quote_path(path) + ": " + std::strerror(errno)));
    }

    return Result<FileDescriptor>::success(FileDescriptor(fd));
}

Result<FileDescriptor> open_for_write(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        return Result<FileDescriptor>::failure(
            make_error(ErrorCode::io_error,
                       "failed to open " + quote_path(path) + ": " + std::strerror(errno)));
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
                make_error(ErrorCode::io_error, "failed to write file: " + std::string(std::strerror(errno))));
        }

        if (result == 0) {
            return Result<bool>::failure(
                make_error(ErrorCode::io_error, "failed to write file: write returned zero"));
        }

        written += static_cast<std::size_t>(result);
    }

    return Result<bool>::success(true);
}

Result<std::uint64_t> copy_file_to_part(const std::filesystem::path& source,
                                        const std::filesystem::path& part_path) {
    auto input = open_for_read(source);
    if (!input) {
        return Result<std::uint64_t>::failure(input.error());
    }

    auto output = open_for_write(part_path);
    if (!output) {
        return Result<std::uint64_t>::failure(output.error());
    }

    std::vector<std::byte> buffer(1024 * 1024);
    std::uint64_t bytes_written = 0;

    while (true) {
        const auto bytes_read = ::read(input.value().get(), buffer.data(), buffer.size());
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }

            return Result<std::uint64_t>::failure(
                make_error(ErrorCode::io_error,
                           "failed to read " + quote_path(source) + ": " + std::strerror(errno)));
        }

        if (bytes_read == 0) {
            break;
        }

        auto written = write_all_file(output.value(), buffer.data(), static_cast<std::size_t>(bytes_read));
        if (!written) {
            return Result<std::uint64_t>::failure(written.error());
        }
        bytes_written += static_cast<std::uint64_t>(bytes_read);
    }

    if (::fsync(output.value().get()) != 0) {
        return Result<std::uint64_t>::failure(
            make_error(ErrorCode::io_error,
                       "failed to fsync " + quote_path(part_path) + ": " + std::strerror(errno)));
    }

    return Result<std::uint64_t>::success(bytes_written);
}

Result<bool> verify_part_hash(const std::filesystem::path& part_path, const ManifestEntry& entry) {
    auto hash = hash_file(part_path);
    if (!hash) {
        return Result<bool>::failure(hash.error());
    }

    if (hash.value().hex_digest != entry.sha256) {
        return Result<bool>::failure(
            make_error(ErrorCode::checksum_mismatch,
                       "synced file sha256 does not match manifest: " +
                           entry.relative_path.generic_string()));
    }

    return Result<bool>::success(true);
}

Result<bool> commit_part_file(const std::filesystem::path& part_path,
                              const std::filesystem::path& target,
                              PartFileGuard& part_file) {
    std::error_code ec;
    std::filesystem::rename(part_path, target, ec);
    if (ec) {
        return Result<bool>::failure(
            make_error(ErrorCode::io_error,
                       "failed to rename " + quote_path(part_path) + " to " +
                           quote_path(target) + ": " + ec.message()));
    }

    part_file.commit();
    return Result<bool>::success(true);
}

Result<std::uint64_t> write_full_file(const std::filesystem::path& source,
                                      const std::filesystem::path& target,
                                      const ManifestEntry& entry) {
    std::filesystem::create_directories(target.parent_path());
    const auto part_path = part_path_for(target);
    PartFileGuard part_file(part_path);

    auto copied = copy_file_to_part(source, part_path);
    if (!copied) {
        return Result<std::uint64_t>::failure(copied.error());
    }

    auto verified = verify_part_hash(part_path, entry);
    if (!verified) {
        return Result<std::uint64_t>::failure(verified.error());
    }

    auto committed = commit_part_file(part_path, target, part_file);
    if (!committed) {
        return Result<std::uint64_t>::failure(committed.error());
    }

    return copied;
}

Result<std::uint64_t> write_delta_file(const std::filesystem::path& source,
                                       const std::filesystem::path& basis,
                                       const std::filesystem::path& target,
                                       const SyncPlanEntry& entry,
                                       std::uint32_t block_size) {
    std::filesystem::create_directories(target.parent_path());
    const auto part_path = part_path_for(target);
    PartFileGuard part_file(part_path);

    auto delta = build_delta(source, entry.basis_signatures, block_size);
    if (!delta) {
        return Result<std::uint64_t>::failure(delta.error());
    }

    auto applied = apply_delta(basis, part_path, delta.value().ops);
    if (!applied) {
        return Result<std::uint64_t>::failure(applied.error());
    }

    auto verified = verify_part_hash(part_path, entry.manifest_entry);
    if (!verified) {
        return Result<std::uint64_t>::failure(verified.error());
    }

    auto committed = commit_part_file(part_path, target, part_file);
    if (!committed) {
        return Result<std::uint64_t>::failure(committed.error());
    }

    return Result<std::uint64_t>::success(entry.manifest_entry.size);
}

}  // namespace

Result<SyncReport> execute_local_sync(const Manifest& source_manifest,
                                      const std::filesystem::path& source_root,
                                      const SyncPlan& plan) {
    SyncReport report;

    for (const auto& entry : plan.entries) {
        const auto source = source_root / entry.manifest_entry.relative_path;
        const auto target = plan.receive_root / entry.manifest_entry.relative_path;

        if (entry.action == SyncAction::skip) {
            ++report.skipped_files;
            continue;
        }

        if (entry.action == SyncAction::full) {
            auto written = write_full_file(source, target, entry.manifest_entry);
            if (!written) {
                return Result<SyncReport>::failure(written.error());
            }
            ++report.full_files;
            report.bytes_written += written.value();
            continue;
        }

        auto written = write_delta_file(source, target, target, entry, plan.block_size);
        if (!written) {
            return Result<SyncReport>::failure(written.error());
        }
        ++report.delta_files;
        report.bytes_written += written.value();
    }

    (void)source_manifest;
    return Result<SyncReport>::success(report);
}

}  // namespace lan
