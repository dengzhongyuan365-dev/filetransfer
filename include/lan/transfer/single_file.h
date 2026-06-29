#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "lan/app/receiver_config.h"
#include "lan/app/sender_config.h"
#include "lan/common/result.h"

namespace lan {

struct SendFileReport {
    std::string file_name;
    std::uint64_t bytes_sent = 0;
    std::string sha256;
    double elapsed_seconds = 0.0;
};

struct ReceiveFileReport {
    std::filesystem::path target_path;
    std::uint64_t bytes_received = 0;
    std::string sha256;
    double elapsed_seconds = 0.0;
};

Result<SendFileReport> send_single_file(const SenderConfig& config);
Result<ReceiveFileReport> receive_single_file(const ReceiverConfig& config);

}  // namespace lan
