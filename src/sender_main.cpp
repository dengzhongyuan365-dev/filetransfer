#include <iostream>
#include <utility>

#include "lan/app/sender_config.h"
#include "lan/common/size.h"
#include "lan/net/tcp.h"
#include "lan/protocol/frame.h"

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

    auto socket = lan::connect_tcp(final_config.target.host, final_config.target.port);
    if (!socket) {
        std::cerr << socket.error().message << '\n';
        return 1;
    }

    lan::Frame hello;
    hello.type = lan::MessageType::hello;
    hello.body = lan::bytes_from_string(final_config.source_path.filename().string());

    auto sent = lan::write_frame(socket.value(), hello);
    if (!sent) {
        std::cerr << sent.error().message << '\n';
        return 1;
    }

    std::cout << "sent frame:\n";
    std::cout << "  type: " << lan::message_type_name(hello.type) << '\n';
    std::cout << "  body: " << lan::body_as_string(hello) << '\n';

    return 0;
}
