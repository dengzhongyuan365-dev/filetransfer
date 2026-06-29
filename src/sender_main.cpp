#include <iostream>
#include <utility>

#include "lan/app/sender_config.h"
#include "lan/common/size.h"
#include "lan/transfer/single_file.h"

int main(int argc, char* argv[]) {
    auto result = lan::parse_sender_args(argc, argv);
    if (!result) {
        std::cerr << result.error().message << '\n';
        std::cerr << lan::sender_usage();
        return 1;
    }

    auto config = std::move(result).value();
    if (config.show_help) {
        std::cout << lan::sender_usage();
        return 0;
    }

    auto validated = lan::validate_sender_config(std::move(config));
    if (!validated) {
        std::cerr << validated.error().message << '\n';
        return 1;
    }

    const auto& final_config = validated.value();

    std::cout << "sender config\n";
    std::cout << "  host: " << final_config.target.host << '\n';
    std::cout << "  port: " << final_config.target.port << '\n';
    std::cout << "  path: " << final_config.source_path.string() << '\n';
    std::cout << "  chunk size: " << lan::format_size(final_config.chunk_size) << " ("
              << final_config.chunk_size << " bytes)\n";
    std::cout << "  resume: " << (final_config.resume ? "true" : "false") << '\n';

    auto sent = lan::send_single_file(final_config);
    if (!sent) {
        std::cerr << sent.error().message << '\n';
        return 1;
    }

    std::cout << "sent file\n";
    std::cout << "  name: " << sent.value().file_name << '\n';
    std::cout << "  size: " << lan::format_size(sent.value().bytes_sent) << '\n';
    std::cout << "  sha256: " << sent.value().sha256 << '\n';

    return 0;
}
