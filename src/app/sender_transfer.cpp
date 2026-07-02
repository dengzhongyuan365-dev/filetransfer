#include "lan/app/sender_transfer.h"

#include <cstdint>
#include <filesystem>
#include <utility>

namespace lan {

namespace {

TransferStarted make_sender_started(std::uint64_t transfer_id,
                                    const SenderConfig& config,
                                    TransferKind kind) {
    return TransferStarted{
        .transfer_id = transfer_id,
        .direction = TransferDirection::send,
        .kind = kind,
        .path = config.source_path,
        .name = config.source_path.filename().string(),
    };
}

TransferFailed make_sender_failed(std::uint64_t transfer_id,
                                  const SenderConfig& config,
                                  TransferKind kind,
                                  const Error& error) {
    return TransferFailed{
        .transfer_id = transfer_id,
        .direction = TransferDirection::send,
        .kind = kind,
        .path = config.source_path,
        .name = config.source_path.filename().string(),
        .error = error,
        .category = error_category(error),
        .retryable = is_retryable(error),
        .user_action_required = needs_user_action(error),
    };
}

TransferCancelled make_sender_cancelled(std::uint64_t transfer_id,
                                        const SenderConfig& config,
                                        TransferKind kind) {
    return TransferCancelled{
        .transfer_id = transfer_id,
        .direction = TransferDirection::send,
        .kind = kind,
        .path = config.source_path,
        .name = config.source_path.filename().string(),
    };
}

TransferCompletionStatus to_completion_status(FileTransferStatus status) {
    switch (status) {
        case FileTransferStatus::transferred:
            return TransferCompletionStatus::transferred;
        case FileTransferStatus::resumed:
            return TransferCompletionStatus::resumed;
        case FileTransferStatus::skipped:
            return TransferCompletionStatus::skipped;
    }

    return TransferCompletionStatus::transferred;
}

TransferCompleted make_sender_file_completed(std::uint64_t transfer_id,
                                             const SenderConfig& config,
                                             const SendFileReport& report) {
    return TransferCompleted{
        .transfer_id = transfer_id,
        .direction = TransferDirection::send,
        .kind = TransferKind::file,
        .path = config.source_path,
        .name = report.file_name,
        .bytes = report.bytes_sent,
        .status = to_completion_status(report.status),
        .resumed_from = report.resumed_from,
        .elapsed_seconds = report.elapsed_seconds,
        .source = report.source,
    };
}

TransferCompleted make_sender_directory_completed(std::uint64_t transfer_id,
                                                  const SenderConfig& config,
                                                  const SendSyncReport& report) {
    return TransferCompleted{
        .transfer_id = transfer_id,
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

TransferProgress make_sender_file_progress(std::uint64_t transfer_id,
                                           const SendFileProgress& progress) {
    return TransferProgress{
        .transfer_id = transfer_id,
        .direction = TransferDirection::send,
        .kind = TransferKind::file,
        .path = progress.source_path,
        .name = progress.file_name,
        .current_bytes = progress.bytes_sent,
        .total_bytes = progress.total_bytes,
        .elapsed_seconds = progress.elapsed_seconds,
    };
}

TransferProgress make_sender_directory_progress(std::uint64_t transfer_id,
                                                const SendSyncProgress& progress) {
    const auto scanning = progress.manifest_files == 0;
    return TransferProgress{
        .transfer_id = transfer_id,
        .direction = TransferDirection::send,
        .kind = TransferKind::directory,
        .path = {},
        .name = {},
        .current_bytes = scanning ? progress.manifest_scanned_bytes
                                  : progress.current_file_bytes,
        .total_bytes = scanning ? 0 : progress.current_file_total_bytes,
        .processed_files = scanning ? progress.manifest_scanned_files
                                    : progress.processed_files,
        .total_files = progress.manifest_files,
        .skipped_files = progress.skipped_files,
        .full_files = progress.full_files,
        .delta_files = progress.delta_files,
        .payload_bytes = progress.delta_payload_bytes_sent,
        .elapsed_seconds = progress.elapsed_seconds,
    };
}

}  // namespace

void SenderTransferEvents::on_file_progress(const SendFileProgress&) {}

void SenderTransferEvents::on_directory_progress(const SendSyncProgress&) {}

SenderTransferRunner::SenderTransferRunner(NetworkBackend& backend) : backend_(backend) {}

Result<SenderTransferReport> SenderTransferRunner::run(const SenderConfig& config,
                                                       SenderTransferEvents& events) {
    cancellation_.reset();
    const auto transfer_id = next_transfer_id_.fetch_add(1);
    const auto kind = std::filesystem::is_directory(config.source_path)
                          ? TransferKind::directory
                          : TransferKind::file;
    events.on_transfer_started(make_sender_started(transfer_id, config, kind));

    auto connection = backend_.connect(config.target.host, config.target.port);
    if (!connection) {
        events.on_transfer_failed(make_sender_failed(transfer_id, config, kind, connection.error()));
        return Result<SenderTransferReport>::failure(connection.error());
    }
    set_active_connection(connection.value().get());

    SenderTransferReport report;
    if (kind == TransferKind::directory) {
        auto synced = sync_sender_to_connection(
            config,
            static_cast<std::uint32_t>(config.chunk_size),
            *connection.value(),
            [transfer_id, &events](const SendSyncProgress& progress) {
                events.on_transfer_progress(
                    make_sender_directory_progress(transfer_id, progress));
                events.on_directory_progress(progress);
            },
            cancellation_);
        if (!synced) {
            clear_active_connection(connection.value().get());
            if (cancellation_.is_cancelled() || synced.error().code == ErrorCode::cancelled) {
                events.on_transfer_cancelled(make_sender_cancelled(transfer_id, config, kind));
                return Result<SenderTransferReport>::failure(
                    Error{ErrorCode::cancelled, "send transfer cancelled"});
            }
            events.on_transfer_failed(make_sender_failed(transfer_id, config, kind, synced.error()));
            return Result<SenderTransferReport>::failure(synced.error());
        }

        report.kind = TransferKind::directory;
        report.directory = std::move(synced).value();
        events.on_transfer_completed(
            make_sender_directory_completed(transfer_id, config, report.directory));
        clear_active_connection(connection.value().get());
        return Result<SenderTransferReport>::success(std::move(report));
    }

    auto sent = send_single_file_to_connection(
        config, *connection.value(), [transfer_id, &events](const SendFileProgress& progress) {
            events.on_transfer_progress(make_sender_file_progress(transfer_id, progress));
            events.on_file_progress(progress);
        }, cancellation_);
    if (!sent) {
        clear_active_connection(connection.value().get());
        if (cancellation_.is_cancelled() || sent.error().code == ErrorCode::cancelled) {
            events.on_transfer_cancelled(make_sender_cancelled(transfer_id, config, kind));
            return Result<SenderTransferReport>::failure(
                Error{ErrorCode::cancelled, "send transfer cancelled"});
        }
        events.on_transfer_failed(make_sender_failed(transfer_id, config, kind, sent.error()));
        return Result<SenderTransferReport>::failure(sent.error());
    }

    report.kind = TransferKind::file;
    report.file = std::move(sent).value();
    events.on_transfer_completed(make_sender_file_completed(transfer_id, config, report.file));
    clear_active_connection(connection.value().get());
    return Result<SenderTransferReport>::success(std::move(report));
}

void SenderTransferRunner::cancel() {
    cancellation_.cancel();

    Connection* connection = nullptr;
    {
        std::lock_guard<std::mutex> lock(active_io_mutex_);
        connection = active_connection_;
    }

    if (connection != nullptr) {
        connection->close();
    }
}

void SenderTransferRunner::set_active_connection(Connection* connection) {
    std::lock_guard<std::mutex> lock(active_io_mutex_);
    active_connection_ = connection;
}

void SenderTransferRunner::clear_active_connection(Connection* connection) {
    std::lock_guard<std::mutex> lock(active_io_mutex_);
    if (active_connection_ == connection) {
        active_connection_ = nullptr;
    }
}

Result<SenderTransferReport> run_sender_transfer(const SenderConfig& config,
                                                 SenderTransferEvents& events) {
    SenderTransferRunner runner;
    return runner.run(config, events);
}

}  // namespace lan
