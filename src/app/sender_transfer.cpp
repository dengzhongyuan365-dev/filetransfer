#include "lan/app/sender_transfer.h"

#include <cstdint>
#include <filesystem>
#include <utility>

namespace lan {

namespace {

TransferStarted make_sender_started(const SenderConfig& config, TransferKind kind) {
    return TransferStarted{
        .direction = TransferDirection::send,
        .kind = kind,
        .path = config.source_path,
        .name = config.source_path.filename().string(),
    };
}

TransferFailed make_sender_failed(const SenderConfig& config, TransferKind kind, const Error& error) {
    return TransferFailed{
        .direction = TransferDirection::send,
        .kind = kind,
        .path = config.source_path,
        .name = config.source_path.filename().string(),
        .error = error,
    };
}

TransferCompleted make_sender_file_completed(const SenderConfig& config,
                                             const SendFileReport& report) {
    return TransferCompleted{
        .direction = TransferDirection::send,
        .kind = TransferKind::file,
        .path = config.source_path,
        .name = report.file_name,
        .bytes = report.bytes_sent,
        .elapsed_seconds = report.elapsed_seconds,
    };
}

TransferCompleted make_sender_directory_completed(const SenderConfig& config,
                                                  const SendSyncReport& report) {
    return TransferCompleted{
        .direction = TransferDirection::send,
        .kind = TransferKind::directory,
        .path = config.source_path,
        .name = config.source_path.filename().string(),
        .total_files = report.manifest_files,
        .skipped_files = report.skipped_files,
        .full_files = report.full_files,
        .delta_files = report.delta_files,
        .payload_bytes = report.delta_payload_bytes_sent,
        .elapsed_seconds = report.elapsed_seconds,
    };
}

}  // namespace

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
    const auto kind = std::filesystem::is_directory(config.source_path)
                          ? TransferKind::directory
                          : TransferKind::file;
    events.on_transfer_started(make_sender_started(config, kind));

    auto connection = backend_.connect(config.target.host, config.target.port);
    if (!connection) {
        events.on_transfer_failed(make_sender_failed(config, kind, connection.error()));
        return Result<SenderTransferReport>::failure(connection.error());
    }

    SenderTransferReport report;
    if (kind == TransferKind::directory) {
        auto synced = sync_sender_to_connection(
            config,
            static_cast<std::uint32_t>(config.chunk_size),
            *connection.value(),
            [&events](const SendSyncProgress& progress) {
                events.on_directory_progress(progress);
            });
        if (!synced) {
            events.on_transfer_failed(make_sender_failed(config, kind, synced.error()));
            return Result<SenderTransferReport>::failure(synced.error());
        }

        report.kind = TransferKind::directory;
        report.directory = std::move(synced).value();
        events.on_transfer_completed(make_sender_directory_completed(config, report.directory));
        return Result<SenderTransferReport>::success(std::move(report));
    }

    auto sent = send_single_file_to_connection(
        config, *connection.value(), [&events](const SendFileProgress& progress) {
            events.on_file_progress(progress);
        });
    if (!sent) {
        events.on_transfer_failed(make_sender_failed(config, kind, sent.error()));
        return Result<SenderTransferReport>::failure(sent.error());
    }

    report.kind = TransferKind::file;
    report.file = std::move(sent).value();
    events.on_transfer_completed(make_sender_file_completed(config, report.file));
    return Result<SenderTransferReport>::success(std::move(report));
}

Result<SenderTransferReport> run_sender_transfer(const SenderConfig& config,
                                                 SenderTransferEvents& events) {
    SenderTransferRunner runner;
    return runner.run(config, events);
}

}  // namespace lan
