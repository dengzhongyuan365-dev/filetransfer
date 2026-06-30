#include "lan/app/receiver_server.h"

#include <utility>

#include "lan/protocol/frame.h"
#include "lan/protocol/hello.h"

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

TransferStarted make_receiver_started(std::uint64_t transfer_id,
                                      const ReceiverConfig& config,
                                      TransferKind kind) {
    return TransferStarted{
        .transfer_id = transfer_id,
        .direction = TransferDirection::receive,
        .kind = kind,
        .path = config.receive_dir,
        .name = {},
    };
}

TransferFailed make_receiver_failed(std::uint64_t transfer_id,
                                     const ReceiverConfig& config,
                                     TransferKind kind,
                                     const Error& error) {
    return TransferFailed{
        .transfer_id = transfer_id,
        .direction = TransferDirection::receive,
        .kind = kind,
        .path = config.receive_dir,
        .name = {},
        .error = error,
        .category = error_category(error),
        .retryable = is_retryable(error),
        .user_action_required = needs_user_action(error),
    };
}

TransferCancelled make_receiver_cancelled(std::uint64_t transfer_id,
                                          const ReceiverConfig& config,
                                          TransferKind kind) {
    return TransferCancelled{
        .transfer_id = transfer_id,
        .direction = TransferDirection::receive,
        .kind = kind,
        .path = config.receive_dir,
        .name = {},
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

TransferCompleted make_receiver_file_completed(std::uint64_t transfer_id,
                                               const ReceiveFileReport& report) {
    return TransferCompleted{
        .transfer_id = transfer_id,
        .direction = TransferDirection::receive,
        .kind = TransferKind::file,
        .path = report.target_path,
        .name = report.target_path.filename().string(),
        .bytes = report.bytes_received,
        .status = to_completion_status(report.status),
        .resumed_from = report.resumed_from,
        .elapsed_seconds = report.elapsed_seconds,
    };
}

TransferCompleted make_receiver_directory_completed(std::uint64_t transfer_id,
                                                    const ReceiverConfig& config,
                                                    const ReceiveSyncReport& report) {
    return TransferCompleted{
        .transfer_id = transfer_id,
        .direction = TransferDirection::receive,
        .kind = TransferKind::directory,
        .path = config.receive_dir,
        .name = config.receive_dir.filename().string(),
        .total_files = report.manifest_files,
        .skipped_files = report.skipped_files,
        .full_files = report.full_files,
        .delta_files = report.delta_files,
        .payload_bytes = report.delta_payload_bytes_received,
        .elapsed_seconds = report.elapsed_seconds,
    };
}

TransferProgress make_receiver_file_progress(std::uint64_t transfer_id,
                                             const ReceiveFileProgress& progress) {
    return TransferProgress{
        .transfer_id = transfer_id,
        .direction = TransferDirection::receive,
        .kind = TransferKind::file,
        .path = progress.target_path,
        .name = progress.file_name,
        .current_bytes = progress.bytes_received,
        .total_bytes = progress.total_bytes,
        .elapsed_seconds = progress.elapsed_seconds,
    };
}

TransferProgress make_receiver_directory_progress(std::uint64_t transfer_id,
                                                  const ReceiveSyncProgress& progress) {
    return TransferProgress{
        .transfer_id = transfer_id,
        .direction = TransferDirection::receive,
        .kind = TransferKind::directory,
        .path = {},
        .name = {},
        .processed_files = progress.processed_files,
        .total_files = progress.manifest_files,
        .skipped_files = progress.skipped_files,
        .full_files = progress.full_files,
        .delta_files = progress.delta_files,
        .payload_bytes = progress.delta_payload_bytes_received,
        .elapsed_seconds = progress.elapsed_seconds,
    };
}

}  // namespace

ReceiverServer::ReceiverServer(NetworkBackend& backend) : backend_(backend) {}

void ReceiverServerEvents::on_file_progress(const ReceiveFileProgress&) {}

void ReceiverServerEvents::on_directory_progress(const ReceiveSyncProgress&) {}

Result<ReceiverServerReport> ReceiverServer::run(const ReceiverConfig& config,
                                                 ReceiverServerEvents& events) {
    stop_requested_.store(false);

    auto listener = backend_.listen(config.bind_address, config.port);
    if (!listener) {
        return Result<ReceiverServerReport>::failure(listener.error());
    }

    set_active_listener(listener.value().get());
    events.on_listening(config);

    ReceiverServerReport report;
    do {
        auto client = listener.value()->accept();
        if (!client) {
            if (stop_requested_.load()) {
                report.stopped = true;
                clear_active_listener(listener.value().get());
                return Result<ReceiverServerReport>::success(report);
            }
            clear_active_listener(listener.value().get());
            return Result<ReceiverServerReport>::failure(client.error());
        }

        set_active_connection(client.value().get());
        auto handled = handle_client(config, *client.value(), events);
        clear_active_connection(client.value().get());
        if (!handled) {
            if (stop_requested_.load() && handled.error().code == ErrorCode::cancelled) {
                report.stopped = true;
                return Result<ReceiverServerReport>::success(report);
            }
            ++report.failed_connections;
            events.on_client_error(handled.error());
            if (config.once) {
                return Result<ReceiverServerReport>::failure(handled.error());
            }
            continue;
        }

        ++report.accepted_connections;
    } while (!config.once && !stop_requested_.load());

    report.stopped = stop_requested_.load();
    clear_active_listener(listener.value().get());
    return Result<ReceiverServerReport>::success(report);
}

void ReceiverServer::stop() {
    stop_requested_.store(true);

    Listener* listener = nullptr;
    Connection* connection = nullptr;
    {
        std::lock_guard<std::mutex> lock(active_io_mutex_);
        listener = active_listener_;
        connection = active_connection_;
    }

    if (listener != nullptr) {
        listener->close();
    }
    if (connection != nullptr) {
        connection->close();
    }
}

Result<bool> ReceiverServer::handle_client(const ReceiverConfig& config,
                                           Connection& connection,
                                           ReceiverServerEvents& events) {
    auto hello = read_frame(connection);
    if (!hello) {
        return Result<bool>::failure(hello.error());
    }

    if (hello.value().type != MessageType::hello) {
        return Result<bool>::failure(
            make_error(ErrorCode::protocol_error, "expected hello frame"));
    }

    auto hello_metadata = decode_hello_frame(hello.value());
    if (!hello_metadata) {
        return Result<bool>::failure(hello_metadata.error());
    }

    const auto transfer_id = next_transfer_id_.fetch_add(1);
    const auto kind = hello_metadata.value().mode == HelloMode::sync ? TransferKind::directory
                                                                     : TransferKind::file;
    events.on_transfer_started(make_receiver_started(transfer_id, config, kind));

    if (kind == TransferKind::directory) {
        auto synced = sync_receiver_from_connection(
            config,
            static_cast<std::uint32_t>(config.block_size),
            connection,
            hello.value(),
            [transfer_id, &events](const ReceiveSyncProgress& progress) {
                events.on_transfer_progress(
                    make_receiver_directory_progress(transfer_id, progress));
                events.on_directory_progress(progress);
            });
        if (!synced) {
            if (stop_requested_.load()) {
                events.on_transfer_cancelled(make_receiver_cancelled(transfer_id, config, kind));
                return Result<bool>::failure(
                    make_error(ErrorCode::cancelled, "receive transfer cancelled"));
            }
            events.on_transfer_failed(make_receiver_failed(transfer_id, config, kind, synced.error()));
            return Result<bool>::failure(synced.error());
        }

        events.on_directory_synced(synced.value());
        events.on_transfer_completed(
            make_receiver_directory_completed(transfer_id, config, synced.value()));
        return Result<bool>::success(true);
    }

    auto received = receive_single_file_from_connection(
        config, connection, hello.value(), [transfer_id, &events](const ReceiveFileProgress& progress) {
            events.on_transfer_progress(make_receiver_file_progress(transfer_id, progress));
            events.on_file_progress(progress);
    });
    if (!received) {
        if (stop_requested_.load()) {
            events.on_transfer_cancelled(make_receiver_cancelled(transfer_id, config, kind));
            return Result<bool>::failure(
                make_error(ErrorCode::cancelled, "receive transfer cancelled"));
        }
        events.on_transfer_failed(make_receiver_failed(transfer_id, config, kind, received.error()));
        return Result<bool>::failure(received.error());
    }

    events.on_file_received(received.value());
    events.on_transfer_completed(make_receiver_file_completed(transfer_id, received.value()));
    return Result<bool>::success(true);
}

void ReceiverServer::set_active_listener(Listener* listener) {
    std::lock_guard<std::mutex> lock(active_io_mutex_);
    active_listener_ = listener;
}

void ReceiverServer::clear_active_listener(Listener* listener) {
    std::lock_guard<std::mutex> lock(active_io_mutex_);
    if (active_listener_ == listener) {
        active_listener_ = nullptr;
    }
}

void ReceiverServer::set_active_connection(Connection* connection) {
    std::lock_guard<std::mutex> lock(active_io_mutex_);
    active_connection_ = connection;
}

void ReceiverServer::clear_active_connection(Connection* connection) {
    std::lock_guard<std::mutex> lock(active_io_mutex_);
    if (active_connection_ == connection) {
        active_connection_ = nullptr;
    }
}

ReceiverServerRunner::ReceiverServerRunner(NetworkBackend& backend) : server_(backend) {}

ReceiverServerRunner::~ReceiverServerRunner() {
    stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

Result<bool> ReceiverServerRunner::start(ReceiverConfig config, ReceiverServerEvents& events) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return Result<bool>::failure(
                make_error(ErrorCode::invalid_argument, "receiver server is already running"));
        }

        if (thread_.joinable()) {
            return Result<bool>::failure(
                make_error(ErrorCode::invalid_argument,
                           "previous receiver server run has not been joined"));
        }

        result_.reset();
        running_ = true;
    }

    thread_ = std::thread([this, config = std::move(config), &events]() mutable {
        auto result = server_.run(config, events);
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(result);
        running_ = false;
    });

    return Result<bool>::success(true);
}

void ReceiverServerRunner::stop() {
    server_.stop();
}

Result<ReceiverServerReport> ReceiverServerRunner::join() {
    if (thread_.joinable()) {
        thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!result_) {
        return Result<ReceiverServerReport>::failure(
            make_error(ErrorCode::internal_error, "receiver server has not been started"));
    }

    auto result = std::move(*result_);
    result_.reset();
    return result;
}

bool ReceiverServerRunner::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

}  // namespace lan
