#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "lan/common/result.h"

namespace lan {

struct ReceiverConfig {
    bool show_help = false;
    std::string bind_address = "0.0.0.0";
    std::uint16_t port = 0;
    std::filesystem::path receive_dir;
    bool allow_overwrite = false;
    bool once = false;
    std::uint64_t block_size = 4 * 1024 * 1024;
};

std::string receiver_usage();
Result<ReceiverConfig> parse_receiver_args(int argc, char* argv[]);
Result<ReceiverConfig> validate_receiver_config(ReceiverConfig config);

}  // namespace lan
