#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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
    std::vector<ManifestEntry> files;
};

Result<Manifest> build_manifest(const std::filesystem::path& root,
                                std::uint64_t hash_buffer_size = 1024 * 1024);

}  // namespace lan
