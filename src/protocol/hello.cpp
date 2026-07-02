#include "lan/protocol/hello.h"

#include <charconv>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

std::string_view mode_name(HelloMode mode) {
    switch (mode) {
        case HelloMode::file:
            return "file";
        case HelloMode::sync:
            return "sync";
    }

    return "file";
}

Result<HelloMode> parse_mode(std::string_view text) {
    if (text == "file") {
        return Result<HelloMode>::success(HelloMode::file);
    }
    if (text == "sync") {
        return Result<HelloMode>::success(HelloMode::sync);
    }

    return Result<HelloMode>::failure(
        make_error(ErrorCode::protocol_error, "unsupported hello mode: " + std::string(text)));
}

Result<std::uint32_t> parse_version(std::string_view text) {
    std::uint32_t version = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, version);
    if (ec != std::errc{} || ptr != end) {
        return Result<std::uint32_t>::failure(
            make_error(ErrorCode::protocol_error, "invalid hello protocol version"));
    }

    return Result<std::uint32_t>::success(version);
}

std::vector<std::string_view> split_tokens(std::string_view text) {
    std::vector<std::string_view> tokens;
    while (!text.empty()) {
        const auto first = text.find_first_not_of(' ');
        if (first == std::string_view::npos) {
            break;
        }
        text.remove_prefix(first);
        const auto next = text.find(' ');
        if (next == std::string_view::npos) {
            tokens.push_back(text);
            break;
        }
        tokens.push_back(text.substr(0, next));
        text.remove_prefix(next + 1);
    }
    return tokens;
}

std::string body_to_string(const std::vector<std::byte>& body) {
    std::string text(body.size(), '\0');
    if (!body.empty()) {
        std::memcpy(text.data(), body.data(), body.size());
    }
    return text;
}

}  // namespace

std::vector<std::byte> encode_hello(const HelloMetadata& metadata) {
    auto text = "lan/" + std::to_string(metadata.protocol_version) + " " +
                std::string(mode_name(metadata.mode));
    if (!metadata.sender_id.empty()) {
        text += " sender=" + metadata.sender_id;
    }
    return bytes_from_string(text);
}

Result<HelloMetadata> decode_hello_body(const std::vector<std::byte>& body) {
    const auto text = body_to_string(body);
    const std::string_view view(text);

    auto legacy_mode = parse_mode(view);
    if (legacy_mode) {
        return Result<HelloMetadata>::success(HelloMetadata{
            .protocol_version = current_hello_version,
            .mode = legacy_mode.value(),
            .sender_id = {},
        });
    }

    static constexpr std::string_view prefix = "lan/";
    if (!view.starts_with(prefix)) {
        return Result<HelloMetadata>::failure(
            make_error(ErrorCode::protocol_error, "invalid hello metadata"));
    }

    const auto mode_separator = view.find(' ');
    if (mode_separator == std::string_view::npos) {
        return Result<HelloMetadata>::failure(
            make_error(ErrorCode::protocol_error, "invalid hello metadata"));
    }

    auto version = parse_version(view.substr(prefix.size(), mode_separator - prefix.size()));
    if (!version) {
        return Result<HelloMetadata>::failure(version.error());
    }
    if (version.value() != current_hello_version) {
        return Result<HelloMetadata>::failure(
            make_error(ErrorCode::protocol_error, "unsupported hello protocol version"));
    }

    const auto tokens = split_tokens(view.substr(mode_separator + 1));
    if (tokens.empty()) {
        return Result<HelloMetadata>::failure(
            make_error(ErrorCode::protocol_error, "invalid hello metadata"));
    }

    auto mode = parse_mode(tokens.front());
    if (!mode) {
        return Result<HelloMetadata>::failure(mode.error());
    }

    std::string sender_id;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        static constexpr std::string_view sender_prefix = "sender=";
        if (tokens[i].starts_with(sender_prefix)) {
            sender_id = std::string(tokens[i].substr(sender_prefix.size()));
        }
    }

    return Result<HelloMetadata>::success(HelloMetadata{
        .protocol_version = version.value(),
        .mode = mode.value(),
        .sender_id = std::move(sender_id),
    });
}

Result<HelloMetadata> decode_hello_frame(const Frame& frame) {
    if (frame.type != MessageType::hello) {
        return Result<HelloMetadata>::failure(
            make_error(ErrorCode::protocol_error, "expected hello frame"));
    }

    return decode_hello_body(frame.body);
}

}  // namespace lan
