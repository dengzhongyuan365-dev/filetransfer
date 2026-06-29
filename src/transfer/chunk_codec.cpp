#include "lan/transfer/chunk_codec.h"

#include <cstring>
#include <utility>

namespace lan {

namespace {

constexpr std::size_t chunk_header_size = 8;

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

void write_u64_be(std::byte* output, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        output[7 - i] = static_cast<std::byte>((value >> (i * 8)) & 0xff);
    }
}

std::uint64_t read_u64_be(const std::byte* input) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < chunk_header_size; ++i) {
        value = (value << 8) | std::to_integer<std::uint64_t>(input[i]);
    }
    return value;
}

}  // namespace

std::vector<std::byte> encode_chunk_body(std::uint64_t offset, const std::byte* data, std::size_t size) {
    std::vector<std::byte> body(chunk_header_size + size);
    write_u64_be(body.data(), offset);
    std::memcpy(body.data() + chunk_header_size, data, size);
    return body;
}

Result<ChunkBodyView> decode_chunk_body(const Frame& frame) {
    if (frame.body.size() < chunk_header_size) {
        return Result<ChunkBodyView>::failure(
            make_error(ErrorCode::protocol_error, "chunk body is missing offset header"));
    }

    return Result<ChunkBodyView>::success(ChunkBodyView{
        .offset = read_u64_be(frame.body.data()),
        .data = frame.body.data() + chunk_header_size,
        .size = frame.body.size() - chunk_header_size,
    });
}

}  // namespace lan
