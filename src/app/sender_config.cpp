#include "lan/app/sender_config.h"

#include <limits>
#include <string_view>

#include "lan/common/path.h"
#include "lan/common/parse.h"

namespace lan {

namespace {

Error invalid_argument(std::string message) {
    return Error{ErrorCode::invalid_argument, std::move(message)};
}

Result<std::string_view> next_value(int argc, char* argv[], int& index, std::string_view option) {
    if (index + 1 >= argc) {
        return Result<std::string_view>::failure(
            invalid_argument("missing value for option: " + std::string(option)));
    }

    std::string_view value = argv[index + 1];
    if (!value.empty() && value.front() == '-') {
        return Result<std::string_view>::failure(
            invalid_argument("missing value for option: " + std::string(option)));
    }

    ++index;
    return Result<std::string_view>::success(value);
}

}  // namespace

std::string sender_usage() {
    return "Usage:\n"
           "  sender --host <ip-or-name> --port <port> --path <file-or-dir> "
           "[--chunk-size <size>] [--resume|--no-resume]\n\n"
           "Examples:\n"
           "  sender --host 192.168.1.20 --port 9000 --path ./demo.bin\n"
           "  sender --host 192.168.1.20 --port 9000 --path ./demo.bin --chunk-size 4MiB\n";
}

Result<SenderConfig> parse_sender_args(int argc, char* argv[]) {
    SenderConfig config;
    bool has_host = false;
    bool has_port = false;
    bool has_path = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            config.show_help = true;
            return Result<SenderConfig>::success(std::move(config));
        }

        if (arg == "--host") {
            auto value = next_value(argc, argv, i, arg);
            if (!value) {
                return Result<SenderConfig>::failure(value.error());
            }
            config.target.host = std::string(value.value());
            has_host = true;
        } else if (arg == "--port") {
            auto value = next_value(argc, argv, i, arg);
            if (!value) {
                return Result<SenderConfig>::failure(value.error());
            }
            auto port = parse_port(value.value());
            if (!port) {
                return Result<SenderConfig>::failure(port.error());
            }
            config.target.port = port.value();
            has_port = true;
        } else if (arg == "--path") {
            auto value = next_value(argc, argv, i, arg);
            if (!value) {
                return Result<SenderConfig>::failure(value.error());
            }
            config.source_path = std::filesystem::path(std::string(value.value()));
            has_path = true;
        } else if (arg == "--chunk-size") {
            auto value = next_value(argc, argv, i, arg);
            if (!value) {
                return Result<SenderConfig>::failure(value.error());
            }
            auto size = parse_size(value.value());
            if (!size) {
                return Result<SenderConfig>::failure(size.error());
            }
            if (size.value() == 0) {
                return Result<SenderConfig>::failure(
                    invalid_argument("chunk size must be greater than zero"));
            }
            config.chunk_size = size.value();
        } else if (arg == "--resume") {
            config.resume = true;
        } else if (arg == "--no-resume") {
            config.resume = false;
        } else {
            return Result<SenderConfig>::failure(
                invalid_argument("unknown option: " + std::string(arg)));
        }
    }

    if (!has_host) {
        return Result<SenderConfig>::failure(invalid_argument("missing required option: --host"));
    }
    if (config.target.host.empty()) {
        return Result<SenderConfig>::failure(invalid_argument("host must not be empty"));
    }
    if (!has_port) {
        return Result<SenderConfig>::failure(invalid_argument("missing required option: --port"));
    }
    if (!has_path) {
        return Result<SenderConfig>::failure(invalid_argument("missing required option: --path"));
    }
    if (config.source_path.empty()) {
        return Result<SenderConfig>::failure(invalid_argument("path must not be empty"));
    }

    return Result<SenderConfig>::success(std::move(config));
}

Result<SenderConfig> validate_sender_config(SenderConfig config) {
    if (config.chunk_size == 0) {
        return Result<SenderConfig>::failure(
            invalid_argument("chunk size must be greater than zero"));
    }
    if (config.chunk_size > std::numeric_limits<std::uint32_t>::max()) {
        return Result<SenderConfig>::failure(
            invalid_argument("chunk size must fit in uint32_t"));
    }

    auto path = require_file_or_directory(config.source_path);
    if (!path) {
        return Result<SenderConfig>::failure(path.error());
    }

    config.source_path = std::move(path).value();
    return Result<SenderConfig>::success(std::move(config));
}

}  // namespace lan
