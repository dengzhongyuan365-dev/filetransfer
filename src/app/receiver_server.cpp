#include "lan/app/receiver_server.h"

#include <utility>

#include "lan/protocol/frame.h"

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

TransferStarted make_receiver_started(const ReceiverConfig& config, TransferKind kind) {
    return TransferStarted{
        .direction = TransferDirection::receive,
        .kind = kind,
        .path = config.receive_dir,
        .name = {},
    };
}

TransferFailed make_receiver_failed(const ReceiverConfig& config,
                                     TransferKind kind,
                                     const Error& error) {
    return TransferFailed{
        .direction = TransferDirection::receive,
        .kind = kind,
        .path = config.receive_dir,
        .name = {},
        .error = error,
    };
}

TransferCompleted make_receiver_file_completed(const ReceiveFileReport& report) {
    return TransferCompleted{
        .direction = TransferDirection::receive,
        .kind = TransferKind::file,
        .path = report.target_path,
        .name = report.target_path.filename().string(),
        .bytes = report.bytes_received,
        .elapsed_seconds = report.elapsed_seconds,
    };
}

TransferCompleted make_receiver_directory_completed(const ReceiverConfig& config,
                                                    const ReceiveSyncReport& report) {
    return TransferCompleted{
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

}  // namespace

ReceiverServer::ReceiverServer(NetworkBackend& backend) : backend_(backend) {}

void ReceiverServerEvents::on_file_progress(const ReceiveFileProgress& progress) {
    on_transfer_progress(TransferProgress{
        .direction = TransferDirection::receive,
        .kind = TransferKind::file,
        .path = progress.target_path,
        .name = progress.file_name,
        .current_bytes = progress.bytes_received,
        .total_bytes = progress.total_bytes,
        .elapsed_seconds = progress.elapsed_seconds,
    });
}

void ReceiverServerEvents::on_directory_progress(const ReceiveSyncProgress& progress) {
    on_transfer_progress(TransferProgress{
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
    });
}

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

        auto handled = handle_client(config, *client.value(), events);
        if (!handled) {
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
    {
        std::lock_guard<std::mutex> lock(listener_mutex_);
        listener = active_listener_;
    }

    if (listener != nullptr) {
        listener->close();
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

    const auto kind = body_as_string(hello.value()) == "sync" ? TransferKind::directory
                                                              : TransferKind::file;
    events.on_transfer_started(make_receiver_started(config, kind));

    if (kind == TransferKind::directory) {
        auto synced = sync_receiver_from_connection(
            config,
            static_cast<std::uint32_t>(config.block_size),
            connection,
            hello.value(),
            [&events](const ReceiveSyncProgress& progress) {
                events.on_directory_progress(progress);
            });
        if (!synced) {
            events.on_transfer_failed(make_receiver_failed(config, kind, synced.error()));
            return Result<bool>::failure(synced.error());
        }

        events.on_directory_synced(synced.value());
        events.on_transfer_completed(make_receiver_directory_completed(config, synced.value()));
        return Result<bool>::success(true);
    }

    auto received = receive_single_file_from_connection(
        config, connection, hello.value(), [&events](const ReceiveFileProgress& progress) {
            events.on_file_progress(progress);
        });
    if (!received) {
        events.on_transfer_failed(make_receiver_failed(config, kind, received.error()));
        return Result<bool>::failure(received.error());
    }

    events.on_file_received(received.value());
    events.on_transfer_completed(make_receiver_file_completed(received.value()));
    return Result<bool>::success(true);
}

void ReceiverServer::set_active_listener(Listener* listener) {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    active_listener_ = listener;
}

void ReceiverServer::clear_active_listener(Listener* listener) {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    if (active_listener_ == listener) {
        active_listener_ = nullptr;
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
