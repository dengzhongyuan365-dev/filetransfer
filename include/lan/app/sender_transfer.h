#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "lan/app/sender_config.h"
#include "lan/app/transfer_event.h"
#include "lan/common/result.h"
#include "lan/net/connection.h"
#include "lan/transfer/single_file.h"
#include "lan/transfer/sync_session.h"

namespace lan {

struct SenderTransferReport {
    TransferKind kind = TransferKind::file;
    SendFileReport file;
    SendSyncReport directory;
};

class SenderTransferEvents : public TransferEvents {
public:
    virtual ~SenderTransferEvents() = default;

    virtual void on_file_progress(const SendFileProgress& progress);
    virtual void on_directory_progress(const SendSyncProgress& progress);
};

class SenderTransferRunner {
public:
    explicit SenderTransferRunner(NetworkBackend& backend = default_network_backend());

    Result<SenderTransferReport> run(const SenderConfig& config, SenderTransferEvents& events);
    void cancel();

private:
    void set_active_connection(Connection* connection);
    void clear_active_connection(Connection* connection);

    NetworkBackend& backend_;
    std::atomic_uint64_t next_transfer_id_ = 1;
    CancellationToken cancellation_;
    std::mutex active_io_mutex_;
    Connection* active_connection_ = nullptr;
};

Result<SenderTransferReport> run_sender_transfer(const SenderConfig& config,
                                                 SenderTransferEvents& events);

}  // namespace lan
