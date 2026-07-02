#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lan/common/result.h"
#include "lan/protocol/frame.h"

namespace lan {

inline constexpr std::uint32_t current_hello_version = 1;

enum class HelloMode {
    file,
    sync,
};

struct HelloMetadata {
    std::uint32_t protocol_version = current_hello_version;
    HelloMode mode = HelloMode::file;
    std::string sender_id;
};

std::vector<std::byte> encode_hello(const HelloMetadata& metadata);
Result<HelloMetadata> decode_hello_body(const std::vector<std::byte>& body);
Result<HelloMetadata> decode_hello_frame(const Frame& frame);

}  // namespace lan
