#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "lan/common/result.h"

namespace lan {

struct FileBeginMetadata {
    std::string name;
    std::uint64_t size = 0;
    std::string sha256;
    bool resume = true;
};

struct FileBeginAckMetadata {
    std::uint64_t offset = 0;
    bool complete = false;
};

std::string encode_file_begin(const FileBeginMetadata& metadata);
Result<FileBeginMetadata> decode_file_begin(std::string_view body);
std::string encode_file_begin_ack(const FileBeginAckMetadata& metadata);
Result<FileBeginAckMetadata> decode_file_begin_ack(std::string_view body);

}  // namespace lan
