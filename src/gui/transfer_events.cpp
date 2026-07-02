#include "gui/transfer_events.h"

#include "gui/qt_utils.h"
#include "lan/common/error.h"

#include <QCoreApplication>

namespace lan::gui {

GuiSenderEvents::GuiSenderEvents(std::function<void(TransferSnapshotStore)> on_change)
    : on_change_(std::move(on_change)) {}

GuiSenderEvents::GuiSenderEvents(std::function<void(TransferSnapshotStore)> on_change,
                                 std::function<void(QString)> on_log)
    : on_change_(std::move(on_change)), on_log_(std::move(on_log)) {}

void GuiSenderEvents::on_transfer_started(const TransferStarted& started) {
    if (started.kind == TransferKind::directory) {
        directory_scan_logged_ = false;
        directory_transfer_logged_ = false;
        current_directory_file_.clear();
        if (on_log_) {
            on_log_(QCoreApplication::translate("MainWindow", "Preparing directory transfer: %1")
                        .arg(to_qstring(started.path)));
        }
    }
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
    if (completed.kind == TransferKind::directory && on_log_) {
        on_log_(QCoreApplication::translate("MainWindow", "Directory transfer completed: %1 files")
                    .arg(completed.total_files));
    }
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

void GuiSenderEvents::on_directory_progress(const SendSyncProgress& progress) {
    if (!on_log_) {
        return;
    }

    if (progress.manifest_files == 0) {
        if (!directory_scan_logged_) {
            on_log_(QCoreApplication::translate("MainWindow", "Scanning directory..."));
            directory_scan_logged_ = true;
        }
        return;
    }

    if (!directory_transfer_logged_) {
        on_log_(QCoreApplication::translate("MainWindow", "Directory scan complete: %1 files. Starting transfer...")
                    .arg(progress.manifest_files));
        directory_transfer_logged_ = true;
    }

    const auto current = progress.current_file.generic_string();
    if (!current.empty() && current != current_directory_file_) {
        current_directory_file_ = current;
        if (progress.current_action == SyncAction::skip) {
            on_log_(QCoreApplication::translate("MainWindow", "Skipping unchanged file: %1")
                        .arg(to_qstring(current_directory_file_)));
        } else {
            on_log_(QCoreApplication::translate("MainWindow", "Sending file: %1")
                        .arg(to_qstring(current_directory_file_)));
        }
    }
}

void GuiSenderEvents::update(const std::function<void()>& apply) {
    std::lock_guard<std::mutex> lock(mutex_);
    apply();
    on_change_(snapshots_);
}

GuiReceiverEvents::GuiReceiverEvents(std::function<void(TransferSnapshotStore, QMap<std::uint64_t, QString>)> on_change,
                                     std::function<void(QString)> on_log,
                                     std::function<void(const ReceiverConfig&)> on_listening)
    : on_change_(std::move(on_change)),
      on_log_(std::move(on_log)),
      on_listening_(std::move(on_listening)) {}

void GuiReceiverEvents::on_listening(const ReceiverConfig& config) {
    if (on_listening_) {
        on_listening_(config);
    }
}

void GuiReceiverEvents::on_client_identified(std::uint64_t transfer_id, const std::string& sender_id) {
    if (sender_id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    peer_ids_.insert(transfer_id, to_qstring(sender_id));
}

void GuiReceiverEvents::on_transfer_started(const TransferStarted& started) {
    if (started.kind == TransferKind::directory) {
        directory_receive_logged_ = false;
        current_directory_file_.clear();
        if (on_log_) {
            on_log_(QCoreApplication::translate("MainWindow", "Preparing to receive directory..."));
        }
    }
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
    on_log_(QCoreApplication::translate("MainWindow", "received %1")
                .arg(to_qstring(report.target_path)));
}

void GuiReceiverEvents::on_directory_progress(const ReceiveSyncProgress& progress) {
    if (!on_log_) {
        return;
    }

    if (!directory_receive_logged_ && progress.manifest_files > 0) {
        on_log_(QCoreApplication::translate("MainWindow", "Receiving directory: %1 files")
                    .arg(progress.manifest_files));
        directory_receive_logged_ = true;
    }

    const auto current = progress.current_file.generic_string();
    if (!current.empty() && current != current_directory_file_) {
        current_directory_file_ = current;
        if (progress.current_action == SyncAction::skip) {
            on_log_(QCoreApplication::translate("MainWindow", "Receiver skipped unchanged file: %1")
                        .arg(to_qstring(current_directory_file_)));
        } else {
            on_log_(QCoreApplication::translate("MainWindow", "Receiver applying file: %1")
                        .arg(to_qstring(current_directory_file_)));
        }
    }
}

void GuiReceiverEvents::on_directory_synced(const ReceiveSyncReport& report) {
    on_log_(QCoreApplication::translate("MainWindow", "synced directory: %1 files")
                .arg(report.manifest_files));
}

void GuiReceiverEvents::on_client_error(const Error& error) {
    on_log_(to_qstring(format_error(error)));
}

void GuiReceiverEvents::update(const std::function<void()>& apply) {
    std::lock_guard<std::mutex> lock(mutex_);
    apply();
    on_change_(snapshots_, peer_ids_);
}

}  // namespace lan::gui
