#pragma once

#include <cstdint>
#include <string_view>

#include "lan/common/result.h"

namespace lan {

Result<std::uint16_t> parse_port(std::string_view text);
Result<std::uint64_t> parse_size(std::string_view text);

}  // namespace lan
