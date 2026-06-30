#include "lan/app/transfer_event.h"

namespace lan {

std::string_view transfer_state_name(TransferState state) {
    switch (state) {
        case TransferState::pending:
            return "pending";
        case TransferState::running:
            return "running";
        case TransferState::completed:
            return "completed";
        case TransferState::failed:
            return "failed";
        case TransferState::cancelled:
            return "cancelled";
    }

    return "failed";
}

bool can_transition(TransferState from, TransferState to) {
    if (from == to) {
        return true;
    }

    switch (from) {
        case TransferState::pending:
            return to == TransferState::running || to == TransferState::cancelled;
        case TransferState::running:
            return to == TransferState::completed || to == TransferState::failed ||
                   to == TransferState::cancelled;
        case TransferState::completed:
        case TransferState::failed:
        case TransferState::cancelled:
            return false;
    }

    return false;
}

void TransferEvents::on_transfer_started(const TransferStarted&) {}

void TransferEvents::on_transfer_progress(const TransferProgress&) {}

void TransferEvents::on_transfer_completed(const TransferCompleted&) {}

void TransferEvents::on_transfer_failed(const TransferFailed&) {}

void TransferEvents::on_transfer_cancelled(const TransferCancelled&) {}

}  // namespace lan
