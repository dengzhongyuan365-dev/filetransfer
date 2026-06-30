#include "lan/app/receiver_server.h"

#include <utility>

#include "lan/protocol/frame.h"

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

}  // namespace

ReceiverServer::ReceiverServer(NetworkBackend& backend) : backend_(backend) {}

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

    if (body_as_string(hello.value()) == "sync") {
        auto synced = sync_receiver_from_connection(
            config, static_cast<std::uint32_t>(config.block_size), connection, hello.value());
        if (!synced) {
            return Result<bool>::failure(synced.error());
        }

        events.on_directory_synced(synced.value());
        return Result<bool>::success(true);
    }

    auto received = receive_single_file_from_connection(config, connection, hello.value());
    if (!received) {
        return Result<bool>::failure(received.error());
    }

    events.on_file_received(received.value());
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

}  // namespace lan
