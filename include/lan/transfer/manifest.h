#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "lan/common/cancellation.h"
#include "lan/common/result.h"

namespace lan {

struct ManifestEntry {
    std::filesystem::path relative_path;
    std::uint64_t size = 0;
    std::string sha256;
    std::uint64_t mtime_ns = 0;
    std::uint32_t mode = 0;
};

struct Manifest {
    std::filesystem::path root;
    std::filesystem::path root_name;
    std::vector<ManifestEntry> files;
};

struct ManifestProgress {
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
};

using ManifestProgressCallback = std::function<void(const ManifestProgress&)>;

Result<Manifest> build_manifest(const std::filesystem::path& root,
                                std::uint64_t hash_buffer_size = 1024 * 1024);
Result<Manifest> build_manifest(const std::filesystem::path& root,
                                std::uint64_t hash_buffer_size,
                                ManifestProgressCallback on_progress,
                                const CancellationToken* cancellation = nullptr);

}  // namespace lan
