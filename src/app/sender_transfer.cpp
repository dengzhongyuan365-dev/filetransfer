#include "lan/app/sender_transfer.h"

#include <cstdint>
#include <filesystem>
#include <utility>

namespace lan {

void SenderTransferEvents::on_file_progress(const SendFileProgress& progress) {
    on_transfer_progress(TransferProgress{
        .direction = TransferDirection::send,
        .kind = TransferKind::file,
        .path = progress.source_path,
        .name = progress.file_name,
        .current_bytes = progress.bytes_sent,
        .total_bytes = progress.total_bytes,
        .elapsed_seconds = progress.elapsed_seconds,
    });
}

void SenderTransferEvents::on_directory_progress(const SendSyncProgress& progress) {
    on_transfer_progress(TransferProgress{
        .direction = TransferDirection::send,
        .kind = TransferKind::directory,
        .path = {},
        .name = {},
        .processed_files = progress.processed_files,
        .total_files = progress.manifest_files,
        .skipped_files = progress.skipped_files,
        .full_files = progress.full_files,
        .delta_files = progress.delta_files,
        .payload_bytes = progress.delta_payload_bytes_sent,
        .elapsed_seconds = progress.elapsed_seconds,
    });
}

SenderTransferRunner::SenderTransferRunner(NetworkBackend& backend) : backend_(backend) {}

Result<SenderTransferReport> SenderTransferRunner::run(const SenderConfig& config,
                                                       SenderTransferEvents& events) {
    auto connection = backend_.connect(config.target.host, config.target.port);
    if (!connection) {
        return Result<SenderTransferReport>::failure(connection.error());
    }

    SenderTransferReport report;
    if (std::filesystem::is_directory(config.source_path)) {
        auto synced = sync_sender_to_connection(
            config,
            static_cast<std::uint32_t>(config.chunk_size),
            *connection.value(),
            [&events](const SendSyncProgress& progress) {
                events.on_directory_progress(progress);
            });
        if (!synced) {
            return Result<SenderTransferReport>::failure(synced.error());
        }

        report.kind = TransferKind::directory;
        report.directory = std::move(synced).value();
        return Result<SenderTransferReport>::success(std::move(report));
    }

    auto sent = send_single_file_to_connection(
        config, *connection.value(), [&events](const SendFileProgress& progress) {
            events.on_file_progress(progress);
        });
    if (!sent) {
        return Result<SenderTransferReport>::failure(sent.error());
    }

    report.kind = TransferKind::file;
    report.file = std::move(sent).value();
    return Result<SenderTransferReport>::success(std::move(report));
}

Result<SenderTransferReport> run_sender_transfer(const SenderConfig& config,
                                                 SenderTransferEvents& events) {
    SenderTransferRunner runner;
    return runner.run(config, events);
}

}  // namespace lan
