#pragma once

#include <string_view>
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

enum class ErrorCategory {
    input,
    filesystem,
    network,
    protocol,
    integrity,
    cancellation,
    internal,
};

struct Error {
    ErrorCode code = ErrorCode::internal_error;
    std::string message;
};

std::string_view error_code_name(ErrorCode code);
std::string_view error_category_name(ErrorCategory category);
ErrorCategory error_category(ErrorCode code);
bool is_retryable(ErrorCode code);
bool is_retryable(const Error& error);
bool needs_user_action(ErrorCode code);
bool needs_user_action(const Error& error);
std::string format_error(const Error& error);

}  // namespace lan
