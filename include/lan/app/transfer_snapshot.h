#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "lan/app/transfer_event.h"

namespace lan {

struct TransferSnapshot {
    std::uint64_t transfer_id = 0;
    TransferState state = TransferState::pending;
    TransferDirection direction = TransferDirection::send;
    TransferKind kind = TransferKind::file;
    std::filesystem::path path;
    std::string name;
    std::uint64_t current_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t processed_files = 0;
    std::uint64_t total_files = 0;
    std::uint64_t skipped_files = 0;
    std::uint64_t full_files = 0;
    std::uint64_t delta_files = 0;
    std::uint64_t payload_bytes = 0;
    TransferCompletionStatus completion_status = TransferCompletionStatus::transferred;
    std::uint64_t resumed_from = 0;
    double elapsed_seconds = 0.0;
    std::optional<Error> error;
    ErrorCategory error_category = ErrorCategory::internal;
    bool retryable = false;
    bool user_action_required = false;
};

class TransferSnapshotTracker {
public:
    const std::optional<TransferSnapshot>& snapshot() const;
    void reset();

    bool apply(const TransferStarted& event);
    bool apply(const TransferProgress& event);
    bool apply(const TransferCompleted& event);
    bool apply(const TransferFailed& event);
    bool apply(const TransferCancelled& event);

private:
    bool can_apply(std::uint64_t transfer_id, TransferState next_state) const;
    bool can_start(std::uint64_t transfer_id, TransferState next_state) const;

    std::optional<TransferSnapshot> snapshot_;
};

class TransferSnapshotStore {
public:
    bool apply(const TransferStarted& event);
    bool apply(const TransferProgress& event);
    bool apply(const TransferCompleted& event);
    bool apply(const TransferFailed& event);
    bool apply(const TransferCancelled& event);

    const TransferSnapshot* find(std::uint64_t transfer_id) const;
    std::vector<TransferSnapshot> snapshots() const;
    void clear();

private:
    std::map<std::uint64_t, TransferSnapshotTracker> trackers_;
};

}  // namespace lan
