#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "lan/common/result.h"

namespace lan {

struct BlockSignature {
    std::uint64_t index = 0;
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t weak_checksum = 0;
    std::string strong_checksum;
};

std::uint32_t weak_checksum(std::span<const std::byte> data);
Result<std::string> sha256_bytes(std::span<const std::byte> data);
Result<std::vector<BlockSignature>> build_block_signatures(const std::filesystem::path& path,
                                                           std::uint32_t block_size);

}  // namespace lan
