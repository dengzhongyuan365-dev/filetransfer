#include <iostream>
#include <utility>

#include "lan/app/receiver_config.h"

int main(int argc, char* argv[]) {
    auto result = lan::parse_receiver_args(argc, argv);
    if (!result) {
        std::cerr << result.error().message << '\n';
        std::cerr << lan::receiver_usage();
        return 1;
    }

    auto config = std::move(result).value();
    if (config.show_help) {
        std::cout << lan::receiver_usage();
        return 0;
    }

    auto validated = lan::validate_receiver_config(std::move(config));
    if (!validated) {
        std::cerr << validated.error().message << '\n';
        return 1;
    }

    const auto& final_config = validated.value();

    std::cout << "receiver config\n";
    std::cout << "  bind: " << final_config.bind_address << '\n';
    std::cout << "  port: " << final_config.port << '\n';
    std::cout << "  dir: " << final_config.receive_dir.string() << '\n';
    std::cout << "  allow overwrite: " << (final_config.allow_overwrite ? "true" : "false") << '\n';

    return 0;
}
