#pragma once

#include <cstdint>
#include <string>

namespace lan {

std::string format_size(std::uint64_t bytes);
std::string format_rate(std::uint64_t bytes, double seconds);

}  // namespace lan
