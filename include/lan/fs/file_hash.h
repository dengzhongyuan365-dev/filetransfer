#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "lan/common/result.h"

namespace lan {

struct FileHash {
    std::string algorithm;
    std::string hex_digest;
    std::uint64_t bytes_hashed = 0;
};

Result<FileHash> hash_file(const std::filesystem::path& path,
                           std::uint64_t buffer_size = 1024 * 1024);

}  // namespace lan
