#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lan/common/result.h"
#include "lan/fs/file_descriptor.h"
#include "lan/net/connection.h"

namespace lan {

inline constexpr std::uint64_t max_frame_body_size = 64ull * 1024ull * 1024ull;

enum class MessageType : std::uint8_t {
    hello = 1,
    file_begin = 2,
    chunk = 3,
    file_end = 4,
    error = 5,
    ack = 6,
    manifest = 7,
    sync_plan = 8,
    delta = 9,
};

struct Frame {
    MessageType type = MessageType::hello;
    std::uint16_t flags = 0;
    std::vector<std::byte> body;
};

std::string message_type_name(MessageType type);
std::vector<std::byte> bytes_from_string(std::string_view text);
std::string body_as_string(const Frame& frame);

Result<bool> write_frame(const FileDescriptor& socket, const Frame& frame);
Result<Frame> read_frame(const FileDescriptor& socket);
Result<bool> write_frame(Connection& connection, const Frame& frame);
Result<Frame> read_frame(Connection& connection);

}  // namespace lan
