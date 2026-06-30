#include "lan/protocol/frame.h"

#include <array>
#include <cstring>

#include "lan/net/tcp.h"

namespace lan {

namespace {

constexpr std::array<char, 4> magic = {'L', 'F', 'T', 'P'};
constexpr std::uint8_t protocol_version = 1;
constexpr std::size_t header_size = 16;

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

void write_u16_be(char* output, std::uint16_t value) {
    output[0] = static_cast<char>((value >> 8) & 0xff);
    output[1] = static_cast<char>(value & 0xff);
}

void write_u64_be(char* output, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        output[7 - i] = static_cast<char>((value >> (i * 8)) & 0xff);
    }
}

std::uint16_t read_u16_be(const char* input) {
    const auto high = static_cast<std::uint16_t>(static_cast<unsigned char>(input[0]));
    const auto low = static_cast<std::uint16_t>(static_cast<unsigned char>(input[1]));
    return static_cast<std::uint16_t>((high << 8) | low);
}

std::uint64_t read_u64_be(const char* input) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<unsigned char>(input[i]);
    }
    return value;
}

bool is_valid_message_type(std::uint8_t value) {
    switch (static_cast<MessageType>(value)) {
        case MessageType::hello:
        case MessageType::file_begin:
        case MessageType::chunk:
        case MessageType::file_end:
        case MessageType::error:
        case MessageType::ack:
        case MessageType::manifest:
        case MessageType::sync_plan:
        case MessageType::delta:
        case MessageType::delta_begin:
        case MessageType::delta_end:
            return true;
    }

    return false;
}

}  // namespace

Result<bool> write_frame_body(auto&& send_bytes, const Frame& frame) {
    if (frame.body.size() > max_frame_body_size) {
        return Result<bool>::failure(
            make_error(ErrorCode::protocol_error, "frame body is too large"));
    }

    std::array<char, header_size> header{};
    std::memcpy(header.data(), magic.data(), magic.size());
    header[4] = static_cast<char>(protocol_version);
    header[5] = static_cast<char>(frame.type);
    write_u16_be(header.data() + 6, frame.flags);
    write_u64_be(header.data() + 8, static_cast<std::uint64_t>(frame.body.size()));

    auto header_result = send_bytes(header.data(), header.size());
    if (!header_result) {
        return Result<bool>::failure(header_result.error());
    }

    if (!frame.body.empty()) {
        const auto* body = reinterpret_cast<const char*>(frame.body.data());
        auto body_result = send_bytes(body, frame.body.size());
        if (!body_result) {
            return Result<bool>::failure(body_result.error());
        }
    }

    return Result<bool>::success(true);
}

Result<Frame> read_frame_body(auto&& recv_bytes) {
    std::array<char, header_size> header{};
    auto header_result = recv_bytes(header.data(), header.size());
    if (!header_result) {
        return Result<Frame>::failure(header_result.error());
    }

    if (!std::equal(magic.begin(), magic.end(), header.begin())) {
        return Result<Frame>::failure(
            make_error(ErrorCode::protocol_error, "invalid frame magic"));
    }

    const auto version = static_cast<std::uint8_t>(header[4]);
    if (version != protocol_version) {
        return Result<Frame>::failure(
            make_error(ErrorCode::protocol_error, "unsupported protocol version"));
    }

    const auto raw_type = static_cast<std::uint8_t>(header[5]);
    if (!is_valid_message_type(raw_type)) {
        return Result<Frame>::failure(
            make_error(ErrorCode::protocol_error, "invalid message type"));
    }

    const auto body_size = read_u64_be(header.data() + 8);
    if (body_size > max_frame_body_size) {
        return Result<Frame>::failure(
            make_error(ErrorCode::protocol_error, "frame body is too large"));
    }

    Frame frame;
    frame.type = static_cast<MessageType>(raw_type);
    frame.flags = read_u16_be(header.data() + 6);
    frame.body.resize(static_cast<std::size_t>(body_size));

    if (!frame.body.empty()) {
        auto* body = reinterpret_cast<char*>(frame.body.data());
        auto body_result = recv_bytes(body, frame.body.size());
        if (!body_result) {
            return Result<Frame>::failure(body_result.error());
        }
    }

    return Result<Frame>::success(std::move(frame));
}

std::string message_type_name(MessageType type) {
    switch (type) {
        case MessageType::hello:
            return "hello";
        case MessageType::file_begin:
            return "file_begin";
        case MessageType::chunk:
            return "chunk";
        case MessageType::file_end:
            return "file_end";
        case MessageType::error:
            return "error";
        case MessageType::ack:
            return "ack";
        case MessageType::manifest:
            return "manifest";
        case MessageType::sync_plan:
            return "sync_plan";
        case MessageType::delta:
            return "delta";
        case MessageType::delta_begin:
            return "delta_begin";
        case MessageType::delta_end:
            return "delta_end";
    }

    return "unknown";
}

std::vector<std::byte> bytes_from_string(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

std::string body_as_string(const Frame& frame) {
    std::string text(frame.body.size(), '\0');
    std::memcpy(text.data(), frame.body.data(), frame.body.size());
    return text;
}

Result<bool> write_frame(const FileDescriptor& socket, const Frame& frame) {
    return write_frame_body(
        [&socket](const char* data, std::size_t size) {
            return send_all(socket, data, size);
        },
        frame);
}

Result<Frame> read_frame(const FileDescriptor& socket) {
    return read_frame_body([&socket](char* data, std::size_t size) {
        return recv_exact(socket, data, size);
    });
}

Result<bool> write_frame(Connection& connection, const Frame& frame) {
    return write_frame_body(
        [&connection](const char* data, std::size_t size) {
            return connection.send_all(data, size);
        },
        frame);
}

Result<Frame> read_frame(Connection& connection) {
    return read_frame_body([&connection](char* data, std::size_t size) {
        return connection.recv_exact(data, size);
    });
}

}  // namespace lan
