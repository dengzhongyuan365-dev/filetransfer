#include <cstdint>
#include <iostream>
#include <utility>

#include "lan/app/sender_config.h"
#include "lan/common/size.h"
#include "lan/transfer/single_file.h"
#include "lan/transfer/sync_session.h"

namespace {

void print_send_file_progress(const lan::SendFileProgress& progress) {
    std::cerr << '\r' << "sending " << progress.file_name << "  "
              << lan::format_size(progress.bytes_sent) << " / "
              << lan::format_size(progress.total_bytes) << "  "
              << lan::format_rate(progress.bytes_sent, progress.elapsed_seconds);
    std::cerr.flush();
}

void print_send_sync_progress(const lan::SendSyncProgress& progress) {
    std::cerr << '\r' << "syncing files " << progress.processed_files << " / "
              << progress.manifest_files << "  skipped " << progress.skipped_files
              << "  full " << progress.full_files << "  delta " << progress.delta_files
              << "  delta frames " << progress.delta_frames_sent;
    std::cerr.flush();
}

}  // namespace

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

    if (std::filesystem::is_directory(final_config.source_path)) {
        auto synced = lan::sync_sender(final_config,
                                       static_cast<std::uint32_t>(final_config.chunk_size),
                                       print_send_sync_progress);
        std::cerr << '\n';
        if (!synced) {
            std::cerr << synced.error().message << '\n';
            return 1;
        }

        std::cout << "synced directory\n";
        std::cout << "  files: " << synced.value().manifest_files << '\n';
        std::cout << "  skipped: " << synced.value().skipped_files << '\n';
        std::cout << "  full: " << synced.value().full_files << '\n';
        std::cout << "  delta: " << synced.value().delta_files << '\n';
        std::cout << "  delta frames sent: " << synced.value().delta_frames_sent << '\n';
        std::cout << "  block size: " << lan::format_size(synced.value().block_size) << '\n';
        return 0;
    }

    auto sent = lan::send_single_file(final_config, print_send_file_progress);
    std::cerr << '\n';
    if (!sent) {
        std::cerr << sent.error().message << '\n';
        return 1;
    }

    std::cout << "sent file\n";
    std::cout << "  name: " << sent.value().file_name << '\n';
    std::cout << "  size: " << lan::format_size(sent.value().bytes_sent) << '\n';
    std::cout << "  elapsed: " << sent.value().elapsed_seconds << " s\n";
    std::cout << "  average speed: "
              << lan::format_rate(sent.value().bytes_sent, sent.value().elapsed_seconds) << '\n';
    std::cout << "  sha256: " << sent.value().sha256 << '\n';

    return 0;
}
