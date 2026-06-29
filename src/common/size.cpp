#include "lan/common/size.h"

#include <array>
#include <iomanip>
#include <sstream>

namespace lan {

std::string format_size(std::uint64_t bytes) {
    static constexpr std::array<const char*, 5> units = {"B", "KiB", "MiB", "GiB", "TiB"};

    auto value = static_cast<double>(bytes);
    std::size_t unit_index = 0;

    while (value >= 1024.0 && unit_index + 1 < units.size()) {
        value /= 1024.0;
        ++unit_index;
    }

    std::ostringstream output;
    if (unit_index == 0) {
        output << bytes << ' ' << units[unit_index];
    } else {
        output << std::fixed << std::setprecision(2) << value << ' ' << units[unit_index];
    }

    return output.str();
}

std::string format_rate(std::uint64_t bytes, double seconds) {
    if (seconds <= 0.0) {
        return "n/a";
    }

    const auto bytes_per_second = static_cast<std::uint64_t>(static_cast<double>(bytes) / seconds);
    return format_size(bytes_per_second) + "/s";
}

}  // namespace lan
