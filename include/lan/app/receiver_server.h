#pragma once

#include <cstdint>

#include "lan/app/receiver_config.h"
#include "lan/common/result.h"
#include "lan/net/connection.h"
#include "lan/transfer/single_file.h"
#include "lan/transfer/sync_session.h"

namespace lan {

struct ReceiverServerReport {
    std::uint64_t accepted_connections = 0;
    std::uint64_t failed_connections = 0;
};

class ReceiverServerEvents {
public:
    virtual ~ReceiverServerEvents() = default;

    virtual void on_listening(const ReceiverConfig& config) = 0;
    virtual void on_file_received(const ReceiveFileReport& report) = 0;
    virtual void on_directory_synced(const ReceiveSyncReport& report) = 0;
    virtual void on_client_error(const Error& error) = 0;
};

class ReceiverServer {
public:
    explicit ReceiverServer(NetworkBackend& backend = default_network_backend());

    Result<ReceiverServerReport> run(const ReceiverConfig& config,
                                     ReceiverServerEvents& events);

private:
    Result<bool> handle_client(const ReceiverConfig& config,
                               Connection& connection,
                               ReceiverServerEvents& events);

    NetworkBackend& backend_;
};

}  // namespace lan
