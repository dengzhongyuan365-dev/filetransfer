#include "lan/common/path.h"

#include <system_error>

namespace lan {

namespace {

Error make_error(ErrorCode code, const std::string& message) {
    return Error{code, message};
}

Error make_filesystem_error(ErrorCode code, const std::string& message, const std::error_code& ec) {
    return Error{code, message + ": " + ec.message()};
}

std::string quote_path(const std::filesystem::path& path) {
    return "'" + path.string() + "'";
}

}  // namespace

Result<std::filesystem::path> normalize_path(const std::filesystem::path& path) {
    if (path.empty()) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::invalid_argument, "path must not be empty"));
    }

    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        return Result<std::filesystem::path>::failure(
            make_filesystem_error(ErrorCode::io_error, "failed to normalize path " + quote_path(path), ec));
    }

    return Result<std::filesystem::path>::success(absolute.lexically_normal());
}

Result<std::filesystem::path> require_file_or_directory(const std::filesystem::path& path) {
    auto normalized = normalize_path(path);
    if (!normalized) {
        return Result<std::filesystem::path>::failure(normalized.error());
    }

    std::error_code ec;
    const auto status = std::filesystem::status(normalized.value(), ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            return Result<std::filesystem::path>::failure(
                make_error(ErrorCode::not_found, "path does not exist: " + quote_path(normalized.value())));
        }

        return Result<std::filesystem::path>::failure(
            make_filesystem_error(ErrorCode::io_error,
                                  "failed to inspect path " + quote_path(normalized.value()),
                                  ec));
    }

    if (!std::filesystem::exists(status)) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::not_found, "path does not exist: " + quote_path(normalized.value())));
    }

    if (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status)) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::invalid_argument,
                       "path must be a regular file or directory: " + quote_path(normalized.value())));
    }

    return Result<std::filesystem::path>::success(std::move(normalized).value());
}

Result<std::filesystem::path> ensure_directory(const std::filesystem::path& path) {
    auto normalized = normalize_path(path);
    if (!normalized) {
        return Result<std::filesystem::path>::failure(normalized.error());
    }

    std::error_code ec;
    if (std::filesystem::exists(normalized.value(), ec)) {
        if (ec) {
            return Result<std::filesystem::path>::failure(
                make_filesystem_error(ErrorCode::io_error,
                                      "failed to inspect directory " + quote_path(normalized.value()),
                                      ec));
        }

        if (!std::filesystem::is_directory(normalized.value(), ec)) {
            if (ec) {
                return Result<std::filesystem::path>::failure(
                    make_filesystem_error(ErrorCode::io_error,
                                          "failed to inspect directory " + quote_path(normalized.value()),
                                          ec));
            }

            return Result<std::filesystem::path>::failure(
                make_error(ErrorCode::invalid_argument,
                           "receive path is not a directory: " + quote_path(normalized.value())));
        }

        return Result<std::filesystem::path>::success(std::move(normalized).value());
    }

    if (ec) {
        return Result<std::filesystem::path>::failure(
            make_filesystem_error(ErrorCode::io_error,
                                  "failed to inspect directory " + quote_path(normalized.value()),
                                  ec));
    }

    if (!std::filesystem::create_directories(normalized.value(), ec) || ec) {
        return Result<std::filesystem::path>::failure(
            make_filesystem_error(ErrorCode::io_error,
                                  "failed to create directory " + quote_path(normalized.value()),
                                  ec));
    }

    return Result<std::filesystem::path>::success(std::move(normalized).value());
}

}  // namespace lan
