#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>

#include "lan/common/error.h"
#include "lan/common/parse.h"
#include "lan/common/size.h"
#include "lan/fs/local_copy.h"

namespace {

std::string usage() {
    return "Usage:\n"
           "  local-copy --source <file> --target <file> [--buffer-size <size>] [--overwrite]\n\n"
           "Examples:\n"
           "  local-copy --source ./demo.bin --target /tmp/demo.bin\n"
           "  local-copy --source ./demo.bin --target /tmp/demo.bin --buffer-size 4MiB\n";
}

lan::Error invalid_argument(std::string message) {
    return lan::Error{lan::ErrorCode::invalid_argument, std::move(message)};
}

lan::Result<std::string_view> next_value(int argc, char* argv[], int& index, std::string_view option) {
    if (index + 1 >= argc) {
        return lan::Result<std::string_view>::failure(
            invalid_argument("missing value for option: " + std::string(option)));
    }

    std::string_view value = argv[index + 1];
    if (!value.empty() && value.front() == '-') {
        return lan::Result<std::string_view>::failure(
            invalid_argument("missing value for option: " + std::string(option)));
    }

    ++index;
    return lan::Result<std::string_view>::success(value);
}

lan::Result<lan::LocalCopyOptions> parse_args(int argc, char* argv[]) {
    lan::LocalCopyOptions options;
    bool has_source = false;
    bool has_target = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            std::cout << usage();
            return lan::Result<lan::LocalCopyOptions>::failure(
                invalid_argument("help requested"));
        }

        if (arg == "--source") {
            auto value = next_value(argc, argv, i, arg);
            if (!value) {
                return lan::Result<lan::LocalCopyOptions>::failure(value.error());
            }
            options.source_path = std::filesystem::path(std::string(value.value()));
            has_source = true;
        } else if (arg == "--target") {
            auto value = next_value(argc, argv, i, arg);
            if (!value) {
                return lan::Result<lan::LocalCopyOptions>::failure(value.error());
            }
            options.target_path = std::filesystem::path(std::string(value.value()));
            has_target = true;
        } else if (arg == "--buffer-size") {
            auto value = next_value(argc, argv, i, arg);
            if (!value) {
                return lan::Result<lan::LocalCopyOptions>::failure(value.error());
            }
            auto size = lan::parse_size(value.value());
            if (!size) {
                return lan::Result<lan::LocalCopyOptions>::failure(size.error());
            }
            options.buffer_size = size.value();
        } else if (arg == "--overwrite") {
            options.overwrite = true;
        } else {
            return lan::Result<lan::LocalCopyOptions>::failure(
                invalid_argument("unknown option: " + std::string(arg)));
        }
    }

    if (!has_source) {
        return lan::Result<lan::LocalCopyOptions>::failure(
            invalid_argument("missing required option: --source"));
    }
    if (!has_target) {
        return lan::Result<lan::LocalCopyOptions>::failure(
            invalid_argument("missing required option: --target"));
    }

    return lan::Result<lan::LocalCopyOptions>::success(std::move(options));
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        std::cout << usage();
        return 0;
    }

    auto parsed = parse_args(argc, argv);
    if (!parsed) {
        std::cerr << lan::format_error(parsed.error()) << '\n';
        std::cerr << usage();
        return 1;
    }

    auto options = std::move(parsed).value();
    options.on_progress = [](const lan::LocalCopyProgress& progress) {
        const double speed = progress.elapsed_seconds > 0.0
                                 ? static_cast<double>(progress.bytes_copied) / progress.elapsed_seconds
                                 : 0.0;

        std::cerr << '\r' << lan::format_size(progress.bytes_copied) << " / "
                  << lan::format_size(progress.total_bytes) << "  "
                  << lan::format_size(static_cast<std::uint64_t>(speed)) << "/s";
    };

    auto result = lan::copy_file(options);
    std::cerr << '\n';

    if (!result) {
        std::cerr << lan::format_error(result.error()) << '\n';
        return 1;
    }

    const auto& report = result.value();
    const double speed = report.elapsed_seconds > 0.0
                             ? static_cast<double>(report.bytes_copied) / report.elapsed_seconds
                             : 0.0;

    std::cout << "copied " << lan::format_size(report.bytes_copied) << " in "
              << report.elapsed_seconds << "s at "
              << lan::format_size(static_cast<std::uint64_t>(speed)) << "/s\n";
    std::cout << report.target_hash.algorithm << ": " << report.target_hash.hex_digest << '\n';

    return 0;
}
