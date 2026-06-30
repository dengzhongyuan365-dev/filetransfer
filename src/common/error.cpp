#include "lan/common/error.h"

#include <string>

namespace lan {

std::string_view error_code_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::invalid_argument:
            return "invalid_argument";
        case ErrorCode::not_found:
            return "not_found";
        case ErrorCode::permission_denied:
            return "permission_denied";
        case ErrorCode::io_error:
            return "io_error";
        case ErrorCode::network_error:
            return "network_error";
        case ErrorCode::protocol_error:
            return "protocol_error";
        case ErrorCode::checksum_mismatch:
            return "checksum_mismatch";
        case ErrorCode::cancelled:
            return "cancelled";
        case ErrorCode::internal_error:
            return "internal_error";
    }

    return "internal_error";
}

std::string_view error_category_name(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::input:
            return "input";
        case ErrorCategory::filesystem:
            return "filesystem";
        case ErrorCategory::network:
            return "network";
        case ErrorCategory::protocol:
            return "protocol";
        case ErrorCategory::integrity:
            return "integrity";
        case ErrorCategory::cancellation:
            return "cancellation";
        case ErrorCategory::internal:
            return "internal";
    }

    return "internal";
}

ErrorCategory error_category(ErrorCode code) {
    switch (code) {
        case ErrorCode::invalid_argument:
            return ErrorCategory::input;
        case ErrorCode::not_found:
        case ErrorCode::permission_denied:
        case ErrorCode::io_error:
            return ErrorCategory::filesystem;
        case ErrorCode::network_error:
            return ErrorCategory::network;
        case ErrorCode::protocol_error:
            return ErrorCategory::protocol;
        case ErrorCode::checksum_mismatch:
            return ErrorCategory::integrity;
        case ErrorCode::cancelled:
            return ErrorCategory::cancellation;
        case ErrorCode::internal_error:
            return ErrorCategory::internal;
    }

    return ErrorCategory::internal;
}

bool is_retryable(ErrorCode code) {
    switch (code) {
        case ErrorCode::network_error:
        case ErrorCode::io_error:
            return true;
        case ErrorCode::invalid_argument:
        case ErrorCode::not_found:
        case ErrorCode::permission_denied:
        case ErrorCode::protocol_error:
        case ErrorCode::checksum_mismatch:
        case ErrorCode::cancelled:
        case ErrorCode::internal_error:
            return false;
    }

    return false;
}

bool is_retryable(const Error& error) {
    return is_retryable(error.code);
}

bool needs_user_action(ErrorCode code) {
    switch (code) {
        case ErrorCode::invalid_argument:
        case ErrorCode::not_found:
        case ErrorCode::permission_denied:
        case ErrorCode::protocol_error:
        case ErrorCode::checksum_mismatch:
            return true;
        case ErrorCode::io_error:
        case ErrorCode::network_error:
        case ErrorCode::cancelled:
        case ErrorCode::internal_error:
            return false;
    }

    return false;
}

bool needs_user_action(const Error& error) {
    return needs_user_action(error.code);
}

std::string format_error(const Error& error) {
    std::string output = "[";
    output += error_code_name(error.code);
    output += "] ";
    output += error.message;
    return output;
}

}  // namespace lan
