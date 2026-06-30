#include "lan/transfer/file_metadata.h"

#include <cstddef>
#include <utility>

#include "lan/common/parse.h"

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

}  // namespace

std::string encode_file_begin(const FileBeginMetadata& metadata) {
    return "name=" + metadata.name + "\n" +
           "size=" + std::to_string(metadata.size) + "\n" +
           "sha256=" + metadata.sha256 + "\n" +
           "resume=" + (metadata.resume ? "1" : "0") + "\n";
}

Result<FileBeginMetadata> decode_file_begin(std::string_view body) {
    FileBeginMetadata metadata;
    bool has_name = false;
    bool has_size = false;
    bool has_sha256 = false;

    std::size_t start = 0;
    while (start < body.size()) {
        const auto end = body.find('\n', start);
        const auto line = body.substr(start, end == std::string_view::npos ? body.size() - start : end - start);

        const auto separator = line.find('=');
        if (separator != std::string_view::npos) {
            const auto key = line.substr(0, separator);
            const auto value = line.substr(separator + 1);

            if (key == "name") {
                metadata.name = std::string(value);
                has_name = true;
            } else if (key == "size") {
                auto parsed = parse_size(value);
                if (!parsed) {
                    return Result<FileBeginMetadata>::failure(parsed.error());
                }
                metadata.size = parsed.value();
                has_size = true;
            } else if (key == "sha256") {
                metadata.sha256 = std::string(value);
                has_sha256 = true;
            } else if (key == "resume") {
                if (value == "1" || value == "true") {
                    metadata.resume = true;
                } else if (value == "0" || value == "false") {
                    metadata.resume = false;
                } else {
                    return Result<FileBeginMetadata>::failure(
                        make_error(ErrorCode::protocol_error, "file_begin has invalid resume value"));
                }
            }
        }

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    if (!has_name || metadata.name.empty()) {
        return Result<FileBeginMetadata>::failure(
            make_error(ErrorCode::protocol_error, "file_begin is missing name"));
    }
    if (!has_size) {
        return Result<FileBeginMetadata>::failure(
            make_error(ErrorCode::protocol_error, "file_begin is missing size"));
    }
    if (!has_sha256 || metadata.sha256.empty()) {
        return Result<FileBeginMetadata>::failure(
            make_error(ErrorCode::protocol_error, "file_begin is missing sha256"));
    }

    return Result<FileBeginMetadata>::success(std::move(metadata));
}

std::string encode_file_begin_ack(const FileBeginAckMetadata& metadata) {
    return "offset=" + std::to_string(metadata.offset) + "\n";
}

Result<FileBeginAckMetadata> decode_file_begin_ack(std::string_view body) {
    if (body == "ready" || body.empty()) {
        return Result<FileBeginAckMetadata>::success(FileBeginAckMetadata{});
    }

    FileBeginAckMetadata metadata;
    bool has_offset = false;

    std::size_t start = 0;
    while (start < body.size()) {
        const auto end = body.find('\n', start);
        const auto line = body.substr(start, end == std::string_view::npos ? body.size() - start : end - start);

        const auto separator = line.find('=');
        if (separator != std::string_view::npos) {
            const auto key = line.substr(0, separator);
            const auto value = line.substr(separator + 1);

            if (key == "offset") {
                auto parsed = parse_size(value);
                if (!parsed) {
                    return Result<FileBeginAckMetadata>::failure(parsed.error());
                }
                metadata.offset = parsed.value();
                has_offset = true;
            }
        }

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    if (!has_offset) {
        return Result<FileBeginAckMetadata>::failure(
            make_error(ErrorCode::protocol_error, "file_begin ack is missing offset"));
    }

    return Result<FileBeginAckMetadata>::success(metadata);
}

}  // namespace lan
