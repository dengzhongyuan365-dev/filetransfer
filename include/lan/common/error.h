#pragma once

#include <string>

namespace lan {

enum class ErrorCode {
    invalid_argument,
    not_found,
    permission_denied,
    io_error,
    network_error,
    protocol_error,
    checksum_mismatch,
    cancelled,
    internal_error,
};

struct Error {
    ErrorCode code = ErrorCode::internal_error;
    std::string message;
};

}  // namespace lan
