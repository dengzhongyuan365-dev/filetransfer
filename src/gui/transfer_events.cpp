#include "gui/transfer_events.h"

#include "gui/qt_utils.h"
#include "lan/common/error.h"

namespace lan::gui {

GuiSenderEvents::GuiSenderEvents(std::function<void(TransferSnapshotStore)> on_change)
    : on_change_(std::move(on_change)) {}

void GuiSenderEvents::on_transfer_started(const TransferStarted& started) {
    update([&] {
        snapshots_.apply(started);
    });
}

void GuiSenderEvents::on_transfer_progress(const TransferProgress& progress) {
    update([&] {
        snapshots_.apply(progress);
    });
}

void GuiSenderEvents::on_transfer_completed(const TransferCompleted& completed) {
    update([&] {
        snapshots_.apply(completed);
    });
}

void GuiSenderEvents::on_transfer_failed(const TransferFailed& failed) {
    update([&] {
        snapshots_.apply(failed);
    });
}

void GuiSenderEvents::on_transfer_cancelled(const TransferCancelled& cancelled) {
    update([&] {
        snapshots_.apply(cancelled);
    });
}

void GuiSenderEvents::update(const std::function<void()>& apply) {
    std::lock_guard<std::mutex> lock(mutex_);
    apply();
    on_change_(snapshots_);
}

GuiReceiverEvents::GuiReceiverEvents(std::function<void(TransferSnapshotStore)> on_change,
                                     std::function<void(QString)> on_log)
    : on_change_(std::move(on_change)), on_log_(std::move(on_log)) {}

void GuiReceiverEvents::on_listening(const ReceiverConfig&) {}

void GuiReceiverEvents::on_transfer_started(const TransferStarted& started) {
    update([&] {
        snapshots_.apply(started);
    });
}

void GuiReceiverEvents::on_transfer_progress(const TransferProgress& progress) {
    update([&] {
        snapshots_.apply(progress);
    });
}

void GuiReceiverEvents::on_transfer_completed(const TransferCompleted& completed) {
    update([&] {
        snapshots_.apply(completed);
    });
}

void GuiReceiverEvents::on_transfer_failed(const TransferFailed& failed) {
    update([&] {
        snapshots_.apply(failed);
    });
}

void GuiReceiverEvents::on_transfer_cancelled(const TransferCancelled& cancelled) {
    update([&] {
        snapshots_.apply(cancelled);
    });
}

void GuiReceiverEvents::on_file_progress(const ReceiveFileProgress&) {}

void GuiReceiverEvents::on_file_received(const ReceiveFileReport& report) {
    on_log_(QString("received %1").arg(to_qstring(report.target_path)));
}

void GuiReceiverEvents::on_directory_progress(const ReceiveSyncProgress&) {}

void GuiReceiverEvents::on_directory_synced(const ReceiveSyncReport& report) {
    on_log_(QString("synced directory: %1 files").arg(report.manifest_files));
}

void GuiReceiverEvents::on_client_error(const Error& error) {
    on_log_(to_qstring(format_error(error)));
}

void GuiReceiverEvents::update(const std::function<void()>& apply) {
    std::lock_guard<std::mutex> lock(mutex_);
    apply();
    on_change_(snapshots_);
}

}  // namespace lan::gui
