#include <iostream>
#include <utility>

#include "lan/app/receiver_config.h"
#include "lan/app/receiver_server.h"
#include "lan/common/size.h"

namespace {

class ConsoleReceiverEvents final : public lan::ReceiverServerEvents {
public:
    void on_listening(const lan::ReceiverConfig& config) override {
        std::cout << (config.once ? "waiting for one transfer...\n"
                                  : "listening for transfers...\n");
        std::cout.flush();
    }

    void on_file_received(const lan::ReceiveFileReport& report) override {
        std::cout << "received file\n";
        std::cout << "  path: " << report.target_path.string() << '\n';
        std::cout << "  size: " << lan::format_size(report.bytes_received) << '\n';
        std::cout << "  elapsed: " << report.elapsed_seconds << " s\n";
        std::cout << "  average speed: "
                  << lan::format_rate(report.bytes_received, report.elapsed_seconds) << '\n';
        std::cout << "  sha256: " << report.sha256 << '\n';
    }

    void on_directory_synced(const lan::ReceiveSyncReport& synced) override {
        std::cout << "synced directory\n";
        std::cout << "  files: " << synced.manifest_files << '\n';
        std::cout << "  skipped: " << synced.skipped_files << '\n';
        std::cout << "  full: " << synced.full_files << '\n';
        std::cout << "  delta: " << synced.delta_files << '\n';
        std::cout << "  files written: " << synced.files_written << '\n';
        std::cout << "  block size: " << lan::format_size(synced.block_size) << '\n';
    }

    void on_client_error(const lan::Error& error) override {
        std::cerr << error.message << '\n';
    }
};

}  // namespace

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
    std::cout << "  once: " << (final_config.once ? "true" : "false") << '\n';
    std::cout << "  block size: " << lan::format_size(final_config.block_size) << " ("
              << final_config.block_size << " bytes)\n";

    ConsoleReceiverEvents events;
    lan::ReceiverServer server;
    auto served = server.run(final_config, events);
    if (!served) {
        std::cerr << served.error().message << '\n';
        return 1;
    }

    return 0;
}
