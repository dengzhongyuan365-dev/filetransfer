#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "lan/common/result.h"
#include "lan/transfer/file_metadata.h"

namespace lan {

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
};

struct SenderConfig {
    bool show_help = false;
    Endpoint target;
    std::filesystem::path source_path;
    std::uint64_t chunk_size = 4 * 1024 * 1024;
    bool resume = true;
    FileTransferSource source = FileTransferSource::file;
    std::string sender_id;
};

std::string sender_usage();
Result<SenderConfig> parse_sender_args(int argc, char* argv[]);
Result<SenderConfig> validate_sender_config(SenderConfig config);

}  // namespace lan
