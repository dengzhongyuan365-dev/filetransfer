#include <iostream>
#include <utility>

#include "lan/app/sender_config.h"
#include "lan/app/sender_transfer.h"
#include "lan/common/error.h"
#include "lan/common/size.h"

namespace {

std::string_view file_status_label(lan::FileTransferStatus status) {
    switch (status) {
        case lan::FileTransferStatus::transferred:
            return "sent file";
        case lan::FileTransferStatus::resumed:
            return "resumed file";
        case lan::FileTransferStatus::skipped:
            return "skipped identical file";
    }

    return "sent file";
}

class ConsoleSenderEvents final : public lan::SenderTransferEvents {
public:
    void on_file_progress(const lan::SendFileProgress& progress) override {
        file_progress_seen_ = true;
        std::cerr << '\r' << "sending " << progress.file_name << "  "
                  << lan::format_size(progress.bytes_sent) << " / "
                  << lan::format_size(progress.total_bytes) << "  "
                  << lan::format_rate(progress.bytes_sent, progress.elapsed_seconds);
        std::cerr.flush();
    }

    void on_directory_progress(const lan::SendSyncProgress& progress) override {
        directory_progress_seen_ = true;
        std::cerr << '\r' << "syncing files " << progress.processed_files << " / "
                  << progress.manifest_files << "  skipped " << progress.skipped_files
                  << "  full " << progress.full_files << "  delta " << progress.delta_files
                  << "  delta frames " << progress.delta_frames_sent
                  << "  payload " << lan::format_size(progress.delta_payload_bytes_sent)
                  << "  elapsed " << progress.elapsed_seconds << " s";
        std::cerr.flush();
    }

    void finish_progress_line() {
        if (file_progress_seen_ || directory_progress_seen_) {
            std::cerr << '\n';
            file_progress_seen_ = false;
            directory_progress_seen_ = false;
        }
    }

private:
    bool file_progress_seen_ = false;
    bool directory_progress_seen_ = false;
};

}  // namespace

int main(int argc, char* argv[]) {
    auto result = lan::parse_sender_args(argc, argv);
    if (!result) {
        std::cerr << lan::format_error(result.error()) << '\n';
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
        std::cerr << lan::format_error(validated.error()) << '\n';
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

    ConsoleSenderEvents events;
    lan::SenderTransferRunner runner;
    auto transferred = runner.run(final_config, events);
    events.finish_progress_line();
    if (!transferred) {
        std::cerr << lan::format_error(transferred.error()) << '\n';
        return 1;
    }

    if (transferred.value().kind == lan::TransferKind::directory) {
        const auto& synced = transferred.value().directory;
        std::cout << "synced directory\n";
        std::cout << "  files: " << synced.manifest_files << '\n';
        std::cout << "  skipped: " << synced.skipped_files << '\n';
        std::cout << "  full: " << synced.full_files << '\n';
        std::cout << "  delta: " << synced.delta_files << '\n';
        std::cout << "  delta frames sent: " << synced.delta_frames_sent << '\n';
        std::cout << "  delta payload sent: "
                  << lan::format_size(synced.delta_payload_bytes_sent) << '\n';
        std::cout << "  block size: " << lan::format_size(synced.block_size) << '\n';
        std::cout << "  elapsed: " << synced.elapsed_seconds << " s\n";
        return 0;
    }

    const auto& sent = transferred.value().file;
    std::cout << file_status_label(sent.status) << '\n';
    std::cout << "  name: " << sent.file_name << '\n';
    std::cout << "  size: " << lan::format_size(sent.bytes_sent) << '\n';
    if (sent.status == lan::FileTransferStatus::resumed) {
        std::cout << "  resumed from: " << lan::format_size(sent.resumed_from) << '\n';
    }
    std::cout << "  elapsed: " << sent.elapsed_seconds << " s\n";
    std::cout << "  average speed: "
              << lan::format_rate(sent.bytes_sent, sent.elapsed_seconds) << '\n';
    std::cout << "  sha256: " << sent.sha256 << '\n';

    return 0;
}
