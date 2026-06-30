#include "lan/transfer/manifest.h"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <sys/stat.h>
#include <utility>

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

std::string quote_path(const std::filesystem::path& path) {
    return "'" + path.string() + "'";
}

bool is_safe_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return false;
    }

    for (const auto& part : path) {
        if (part == "." || part == "..") {
            return false;
        }
    }

    return true;
}

std::uint64_t file_mtime_ns(const std::filesystem::path& path) {
    std::error_code ec;
    const auto file_time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return 0;
    }

    const auto system_time = std::chrono::time_point_cast<std::chrono::nanoseconds>(
        file_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const auto count = system_time.time_since_epoch().count();
    if (count < 0) {
        return 0;
    }

    return static_cast<std::uint64_t>(count);
}

std::uint32_t file_mode(const std::filesystem::path& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return 0;
    }

    return static_cast<std::uint32_t>(info.st_mode & 07777);
}

Result<ManifestEntry> build_entry(const std::filesystem::path& root,
                                  const std::filesystem::path& file) {
    std::error_code ec;
    auto relative = std::filesystem::relative(file, root, ec);
    if (ec || !is_safe_relative_path(relative)) {
        return Result<ManifestEntry>::failure(
            make_error(ErrorCode::invalid_argument,
                       "failed to build safe relative path for " + quote_path(file)));
    }

    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        return Result<ManifestEntry>::failure(
            make_error(ErrorCode::io_error,
                       "failed to read file size " + quote_path(file) + ": " + ec.message()));
    }

    return Result<ManifestEntry>::success(ManifestEntry{
        .relative_path = relative.lexically_normal(),
        .size = size,
        .sha256 = {},
        .mtime_ns = file_mtime_ns(file),
        .mode = file_mode(file),
    });
}

}  // namespace

Result<Manifest> build_manifest(const std::filesystem::path& root, std::uint64_t hash_buffer_size) {
    return build_manifest(root, hash_buffer_size, ManifestProgressCallback{}, nullptr);
}

Result<Manifest> build_manifest(const std::filesystem::path& root,
                                std::uint64_t hash_buffer_size,
                                ManifestProgressCallback on_progress,
                                const CancellationToken* cancellation) {
    (void)hash_buffer_size;

    std::error_code ec;
    const auto root_status = std::filesystem::symlink_status(root, ec);
    if (ec) {
        return Result<Manifest>::failure(
            make_error(ErrorCode::io_error,
                       "failed to inspect manifest root " + quote_path(root) + ": " + ec.message()));
    }

    if (std::filesystem::is_symlink(root_status)) {
        return Result<Manifest>::failure(
            make_error(ErrorCode::invalid_argument, "manifest root must not be a symlink"));
    }

    if (!std::filesystem::is_directory(root_status)) {
        return Result<Manifest>::failure(
            make_error(ErrorCode::invalid_argument, "manifest root must be a directory"));
    }

    Manifest manifest;
    manifest.root = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        manifest.root = root.lexically_normal();
    }

    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        return Result<Manifest>::failure(
            make_error(ErrorCode::io_error,
                       "failed to scan manifest root " + quote_path(root) + ": " + ec.message()));
    }

    const std::filesystem::recursive_directory_iterator end;
    std::uint64_t scanned_bytes = 0;
    for (; it != end; it.increment(ec)) {
        if (cancellation != nullptr && cancellation->is_cancelled()) {
            return Result<Manifest>::failure(
                make_error(ErrorCode::cancelled, "manifest scan cancelled"));
        }

        if (ec) {
            return Result<Manifest>::failure(
                make_error(ErrorCode::io_error, "failed while scanning manifest: " + ec.message()));
        }

        const auto status = it->symlink_status(ec);
        if (ec) {
            return Result<Manifest>::failure(
                make_error(ErrorCode::io_error,
                           "failed to inspect " + quote_path(it->path()) + ": " + ec.message()));
        }

        if (std::filesystem::is_symlink(status)) {
            if (std::filesystem::is_directory(status)) {
                it.disable_recursion_pending();
            }
            continue;
        }

        if (!std::filesystem::is_regular_file(status)) {
            continue;
        }

        auto entry = build_entry(root, it->path());
        if (!entry) {
            return Result<Manifest>::failure(entry.error());
        }
        scanned_bytes += entry.value().size;
        manifest.files.push_back(std::move(entry).value());
        if (on_progress) {
            on_progress(ManifestProgress{
                .files = static_cast<std::uint64_t>(manifest.files.size()),
                .bytes = scanned_bytes,
            });
        }
    }

    std::sort(manifest.files.begin(), manifest.files.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.relative_path.generic_string() < rhs.relative_path.generic_string();
    });

    return Result<Manifest>::success(std::move(manifest));
}

}  // namespace lan
