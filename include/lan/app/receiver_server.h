#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

#include "lan/app/receiver_config.h"
#include "lan/app/transfer_event.h"
#include "lan/common/result.h"
#include "lan/net/connection.h"
#include "lan/transfer/single_file.h"
#include "lan/transfer/sync_session.h"

namespace lan {

struct ReceiverServerReport {
    std::uint64_t accepted_connections = 0;
    std::uint64_t failed_connections = 0;
    bool stopped = false;
};

class ReceiverServerEvents : public TransferEvents {
public:
    virtual ~ReceiverServerEvents() = default;

    virtual void on_listening(const ReceiverConfig& config) = 0;
    virtual void on_file_progress(const ReceiveFileProgress& progress);
    virtual void on_file_received(const ReceiveFileReport& report) = 0;
    virtual void on_directory_progress(const ReceiveSyncProgress& progress);
    virtual void on_directory_synced(const ReceiveSyncReport& report) = 0;
    virtual void on_client_error(const Error& error) = 0;
};

class ReceiverServer {
public:
    explicit ReceiverServer(NetworkBackend& backend = default_network_backend());

    Result<ReceiverServerReport> run(const ReceiverConfig& config,
                                     ReceiverServerEvents& events);
    void stop();

private:
    Result<bool> handle_client(const ReceiverConfig& config,
                               Connection& connection,
                               ReceiverServerEvents& events);
    void set_active_listener(Listener* listener);
    void clear_active_listener(Listener* listener);
    void set_active_connection(Connection* connection);
    void clear_active_connection(Connection* connection);

    NetworkBackend& backend_;
    std::atomic_bool stop_requested_ = false;
    std::atomic_uint64_t next_transfer_id_ = 1;
    std::mutex active_io_mutex_;
    Listener* active_listener_ = nullptr;
    Connection* active_connection_ = nullptr;
};

class ReceiverServerRunner {
public:
    explicit ReceiverServerRunner(NetworkBackend& backend = default_network_backend());
    ~ReceiverServerRunner();

    ReceiverServerRunner(const ReceiverServerRunner&) = delete;
    ReceiverServerRunner& operator=(const ReceiverServerRunner&) = delete;

    Result<bool> start(ReceiverConfig config, ReceiverServerEvents& events);
    void stop();
    Result<ReceiverServerReport> join();
    bool running() const;

private:
    ReceiverServer server_;
    mutable std::mutex mutex_;
    std::thread thread_;
    bool running_ = false;
    std::optional<Result<ReceiverServerReport>> result_;
};

}  // namespace lan
