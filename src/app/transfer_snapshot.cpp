#include "lan/app/transfer_snapshot.h"

#include <utility>

namespace lan {

const std::optional<TransferSnapshot>& TransferSnapshotTracker::snapshot() const {
    return snapshot_;
}

void TransferSnapshotTracker::reset() {
    snapshot_.reset();
}

bool TransferSnapshotTracker::can_apply(std::uint64_t transfer_id,
                                        TransferState next_state) const {
    if (!snapshot_) {
        return false;
    }

    return snapshot_->transfer_id == transfer_id &&
           can_transition(snapshot_->state, next_state);
}

bool TransferSnapshotTracker::can_start(std::uint64_t, TransferState next_state) const {
    return !snapshot_ && next_state == TransferState::running;
}

bool TransferSnapshotTracker::apply(const TransferStarted& event) {
    if (!can_start(event.transfer_id, event.state)) {
        return false;
    }

    TransferSnapshot snapshot;
    snapshot.transfer_id = event.transfer_id;
    snapshot.state = event.state;
    snapshot.direction = event.direction;
    snapshot.kind = event.kind;
    snapshot.path = event.path;
    snapshot.name = event.name;
    snapshot_ = std::move(snapshot);
    return true;
}

bool TransferSnapshotTracker::apply(const TransferProgress& event) {
    if (!can_apply(event.transfer_id, event.state)) {
        return false;
    }

    snapshot_->state = event.state;
    snapshot_->direction = event.direction;
    snapshot_->kind = event.kind;
    snapshot_->path = event.path.empty() ? snapshot_->path : event.path;
    snapshot_->name = event.name.empty() ? snapshot_->name : event.name;
    snapshot_->current_bytes = event.current_bytes;
    snapshot_->total_bytes = event.total_bytes;
    snapshot_->processed_files = event.processed_files;
    snapshot_->total_files = event.total_files;
    snapshot_->skipped_files = event.skipped_files;
    snapshot_->full_files = event.full_files;
    snapshot_->delta_files = event.delta_files;
    snapshot_->payload_bytes = event.payload_bytes;
    snapshot_->elapsed_seconds = event.elapsed_seconds;
    return true;
}

bool TransferSnapshotTracker::apply(const TransferCompleted& event) {
    if (!can_apply(event.transfer_id, event.state)) {
        return false;
    }

    snapshot_->state = event.state;
    snapshot_->direction = event.direction;
    snapshot_->kind = event.kind;
    snapshot_->path = event.path.empty() ? snapshot_->path : event.path;
    snapshot_->name = event.name.empty() ? snapshot_->name : event.name;
    snapshot_->current_bytes = event.bytes;
    snapshot_->total_bytes = event.bytes;
    snapshot_->total_files = event.total_files;
    snapshot_->skipped_files = event.skipped_files;
    snapshot_->full_files = event.full_files;
    snapshot_->delta_files = event.delta_files;
    snapshot_->payload_bytes = event.payload_bytes;
    snapshot_->completion_status = event.status;
    snapshot_->resumed_from = event.resumed_from;
    snapshot_->elapsed_seconds = event.elapsed_seconds;
    snapshot_->error.reset();
    snapshot_->retryable = false;
    snapshot_->user_action_required = false;
    return true;
}

bool TransferSnapshotTracker::apply(const TransferFailed& event) {
    if (!can_apply(event.transfer_id, event.state)) {
        return false;
    }

    snapshot_->state = event.state;
    snapshot_->direction = event.direction;
    snapshot_->kind = event.kind;
    snapshot_->path = event.path.empty() ? snapshot_->path : event.path;
    snapshot_->name = event.name.empty() ? snapshot_->name : event.name;
    snapshot_->error = event.error;
    snapshot_->error_category = event.category;
    snapshot_->retryable = event.retryable;
    snapshot_->user_action_required = event.user_action_required;
    return true;
}

bool TransferSnapshotTracker::apply(const TransferCancelled& event) {
    if (!can_apply(event.transfer_id, event.state)) {
        return false;
    }

    snapshot_->state = event.state;
    snapshot_->direction = event.direction;
    snapshot_->kind = event.kind;
    snapshot_->path = event.path.empty() ? snapshot_->path : event.path;
    snapshot_->name = event.name.empty() ? snapshot_->name : event.name;
    snapshot_->error.reset();
    snapshot_->retryable = false;
    snapshot_->user_action_required = false;
    return true;
}

}  // namespace lan
