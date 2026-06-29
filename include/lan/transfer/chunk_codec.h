#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lan/common/result.h"
#include "lan/protocol/frame.h"

namespace lan {

struct ChunkBodyView {
    std::uint64_t offset = 0;
    const std::byte* data = nullptr;
    std::size_t size = 0;
};

std::vector<std::byte> encode_chunk_body(std::uint64_t offset, const std::byte* data, std::size_t size);
Result<ChunkBodyView> decode_chunk_body(const Frame& frame);

}  // namespace lan
