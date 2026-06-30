#pragma once

#include <QString>

#include <functional>
#include <mutex>

#include "lan/app/receiver_server.h"
#include "lan/app/sender_transfer.h"
#include "lan/app/transfer_snapshot.h"

namespace lan::gui {

class GuiSenderEvents final : public SenderTransferEvents {
public:
    explicit GuiSenderEvents(std::function<void(TransferSnapshotStore)> on_change);

    void on_transfer_started(const TransferStarted& started) override;
    void on_transfer_progress(const TransferProgress& progress) override;
    void on_transfer_completed(const TransferCompleted& completed) override;
    void on_transfer_failed(const TransferFailed& failed) override;
    void on_transfer_cancelled(const TransferCancelled& cancelled) override;

private:
    void update(const std::function<void()>& apply);

    std::mutex mutex_;
    TransferSnapshotStore snapshots_;
    std::function<void(TransferSnapshotStore)> on_change_;
};

class GuiReceiverEvents final : public ReceiverServerEvents {
public:
    GuiReceiverEvents(std::function<void(TransferSnapshotStore)> on_change,
                      std::function<void(QString)> on_log);

    void on_listening(const ReceiverConfig& config) override;
    void on_transfer_started(const TransferStarted& started) override;
    void on_transfer_progress(const TransferProgress& progress) override;
    void on_transfer_completed(const TransferCompleted& completed) override;
    void on_transfer_failed(const TransferFailed& failed) override;
    void on_transfer_cancelled(const TransferCancelled& cancelled) override;
    void on_file_progress(const ReceiveFileProgress& progress) override;
    void on_file_received(const ReceiveFileReport& report) override;
    void on_directory_progress(const ReceiveSyncProgress& progress) override;
    void on_directory_synced(const ReceiveSyncReport& report) override;
    void on_client_error(const Error& error) override;

private:
    void update(const std::function<void()>& apply);

    std::mutex mutex_;
    TransferSnapshotStore snapshots_;
    std::function<void(TransferSnapshotStore)> on_change_;
    std::function<void(QString)> on_log_;
};

}  // namespace lan::gui
