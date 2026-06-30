#include <iostream>
#include <utility>

#include "lan/app/receiver_config.h"
#include "lan/common/size.h"
#include "lan/net/connection.h"
#include "lan/protocol/frame.h"
#include "lan/transfer/single_file.h"
#include "lan/transfer/sync_session.h"

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

    std::cout << "waiting for one transfer...\n";
    std::cout.flush();

    auto listener = lan::default_network_backend().listen(final_config.bind_address, final_config.port);
    if (!listener) {
        std::cerr << listener.error().message << '\n';
        return 1;
    }

    auto client = listener.value()->accept();
    if (!client) {
        std::cerr << client.error().message << '\n';
        return 1;
    }

    auto hello = lan::read_frame(*client.value());
    if (!hello) {
        std::cerr << hello.error().message << '\n';
        return 1;
    }

    if (hello.value().type != lan::MessageType::hello) {
        std::cerr << "expected hello frame\n";
        return 1;
    }

    if (lan::body_as_string(hello.value()) == "sync") {
        auto synced = lan::sync_receiver_from_connection(
            final_config, 4 * 1024 * 1024, *client.value(), hello.value());
        if (!synced) {
            std::cerr << synced.error().message << '\n';
            return 1;
        }

        std::cout << "synced directory\n";
        std::cout << "  files: " << synced.value().manifest_files << '\n';
        std::cout << "  skipped: " << synced.value().skipped_files << '\n';
        std::cout << "  full: " << synced.value().full_files << '\n';
        std::cout << "  delta: " << synced.value().delta_files << '\n';
        std::cout << "  files written: " << synced.value().files_written << '\n';
        std::cout << "  block size: " << lan::format_size(synced.value().block_size) << '\n';
        return 0;
    }

    auto received = lan::receive_single_file_from_connection(final_config, *client.value(), hello.value());
    if (received) {
        std::cout << "received file\n";
        std::cout << "  path: " << received.value().target_path.string() << '\n';
        std::cout << "  size: " << lan::format_size(received.value().bytes_received) << '\n';
        std::cout << "  elapsed: " << received.value().elapsed_seconds << " s\n";
        std::cout << "  average speed: "
                  << lan::format_rate(received.value().bytes_received, received.value().elapsed_seconds) << '\n';
        std::cout << "  sha256: " << received.value().sha256 << '\n';

        return 0;
    }

    std::cerr << received.error().message << '\n';
    return 1;

}
