#include "lan/app/receiver_config.h"

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

std::string receiver_usage() {
    return "Usage:\n"
           "  receiver --port <port> --dir <receive-dir> [--bind <address>] "
           "[--allow-overwrite] [--once] [--block-size <size>]\n\n"
           "Examples:\n"
           "  receiver --port 9000 --dir ~/Downloads/reviewdir\n"
           "  receiver --bind 0.0.0.0 --port 9000 --dir ~/Downloads/reviewdir\n"
           "  receiver --port 9000 --dir ~/Downloads/reviewdir --once\n"
           "  receiver --port 9000 --dir ~/Downloads/reviewdir --block-size 1MiB\n";
}

Result<ReceiverConfig> parse_receiver_args(int argc, char* argv[]) {
    ReceiverConfig config;
    bool has_port = false;
    bool has_dir = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            config.show_help = true;
            return Result<ReceiverConfig>::success(std::move(config));
        }

        if (arg == "--bind") {
            auto value = next_value(argc, argv, i, arg);
            if (!value) {
                return Result<ReceiverConfig>::failure(value.error());
            }
            config.bind_address = std::string(value.value());
        } else if (arg == "--port") {
            auto value = next_value(argc, argv, i, arg);
            if (!value) {
                return Result<ReceiverConfig>::failure(value.error());
            }
            auto port = parse_port(value.value());
            if (!port) {
                return Result<ReceiverConfig>::failure(port.error());
            }
            config.port = port.value();
            has_port = true;
        } else if (arg == "--dir") {
            auto value = next_value(argc, argv, i, arg);
            if (!value) {
                return Result<ReceiverConfig>::failure(value.error());
            }
            config.receive_dir = std::filesystem::path(std::string(value.value()));
            has_dir = true;
        } else if (arg == "--allow-overwrite") {
            config.allow_overwrite = true;
        } else if (arg == "--once") {
            config.once = true;
        } else if (arg == "--block-size") {
            auto value = next_value(argc, argv, i, arg);
            if (!value) {
                return Result<ReceiverConfig>::failure(value.error());
            }
            auto size = parse_size(value.value());
            if (!size) {
                return Result<ReceiverConfig>::failure(size.error());
            }
            if (size.value() == 0) {
                return Result<ReceiverConfig>::failure(
                    invalid_argument("block size must be greater than zero"));
            }
            config.block_size = size.value();
        } else {
            return Result<ReceiverConfig>::failure(
                invalid_argument("unknown option: " + std::string(arg)));
        }
    }

    if (!has_port) {
        return Result<ReceiverConfig>::failure(invalid_argument("missing required option: --port"));
    }
    if (!has_dir) {
        return Result<ReceiverConfig>::failure(invalid_argument("missing required option: --dir"));
    }
    if (config.bind_address.empty()) {
        return Result<ReceiverConfig>::failure(invalid_argument("bind address must not be empty"));
    }
    if (config.receive_dir.empty()) {
        return Result<ReceiverConfig>::failure(invalid_argument("receive dir must not be empty"));
    }

    return Result<ReceiverConfig>::success(std::move(config));
}

Result<ReceiverConfig> validate_receiver_config(ReceiverConfig config) {
    if (config.block_size == 0) {
        return Result<ReceiverConfig>::failure(
            invalid_argument("block size must be greater than zero"));
    }

    auto dir = ensure_directory(config.receive_dir);
    if (!dir) {
        return Result<ReceiverConfig>::failure(dir.error());
    }

    config.receive_dir = std::move(dir).value();
    if (config.block_size > std::numeric_limits<std::uint32_t>::max()) {
        return Result<ReceiverConfig>::failure(
            invalid_argument("block size must fit in uint32_t"));
    }

    return Result<ReceiverConfig>::success(std::move(config));
}

}  // namespace lan
