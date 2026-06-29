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
};

std::string encode_file_begin(const FileBeginMetadata& metadata);
Result<FileBeginMetadata> decode_file_begin(std::string_view body);

}  // namespace lan
