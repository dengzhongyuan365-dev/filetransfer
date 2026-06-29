#include "lan/common/parse.h"

#include <charconv>
#include <cctype>
#include <limits>
#include <string>

namespace lan {

namespace {

Error invalid_argument(std::string message) {
    return Error{ErrorCode::invalid_argument, std::move(message)};
}

std::string to_string(std::string_view text) {
    return std::string(text.begin(), text.end());
}

Result<std::uint64_t> parse_u64(std::string_view text) {
    if (text.empty()) {
        return Result<std::uint64_t>::failure(invalid_argument("expected a number"));
    }

    std::uint64_t value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return Result<std::uint64_t>::failure(
            invalid_argument("invalid number: " + to_string(text)));
    }

    return Result<std::uint64_t>::success(value);
}

std::string lower_ascii(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char ch : text) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

}  // namespace

Result<std::uint16_t> parse_port(std::string_view text) {
    auto parsed = parse_u64(text);
    if (!parsed) {
        return Result<std::uint16_t>::failure(parsed.error());
    }

    const auto value = parsed.value();
    if (value == 0 || value > std::numeric_limits<std::uint16_t>::max()) {
        return Result<std::uint16_t>::failure(
            invalid_argument("port out of range: " + to_string(text)));
    }

    return Result<std::uint16_t>::success(static_cast<std::uint16_t>(value));
}

Result<std::uint64_t> parse_size(std::string_view text) {
    if (text.empty()) {
        return Result<std::uint64_t>::failure(invalid_argument("size is empty"));
    }

    std::size_t number_end = 0;
    while (number_end < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[number_end])) != 0) {
        ++number_end;
    }

    if (number_end == 0) {
        return Result<std::uint64_t>::failure(
            invalid_argument("size must start with a number: " + to_string(text)));
    }

    const auto number_text = text.substr(0, number_end);
    const auto suffix = lower_ascii(text.substr(number_end));

    std::uint64_t multiplier = 1;
    if (suffix.empty() || suffix == "b") {
        multiplier = 1;
    } else if (suffix == "k" || suffix == "kb" || suffix == "kib") {
        multiplier = 1024ull;
    } else if (suffix == "m" || suffix == "mb" || suffix == "mib") {
        multiplier = 1024ull * 1024ull;
    } else if (suffix == "g" || suffix == "gb" || suffix == "gib") {
        multiplier = 1024ull * 1024ull * 1024ull;
    } else {
        return Result<std::uint64_t>::failure(
            invalid_argument("invalid size suffix: " + suffix));
    }

    auto parsed = parse_u64(number_text);
    if (!parsed) {
        return Result<std::uint64_t>::failure(parsed.error());
    }

    if (parsed.value() > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        return Result<std::uint64_t>::failure(
            invalid_argument("size is too large: " + to_string(text)));
    }

    return Result<std::uint64_t>::success(parsed.value() * multiplier);
}

}  // namespace lan
