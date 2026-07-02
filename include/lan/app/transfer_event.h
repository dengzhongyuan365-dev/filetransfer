#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "lan/common/error.h"
#include "lan/transfer/file_metadata.h"

namespace lan {

enum class TransferDirection {
    send,
    receive,
};

enum class TransferKind {
    file,
    directory,
};

enum class TransferCompletionStatus {
    transferred,
    resumed,
    skipped,
};

enum class TransferState {
    pending,
    paused,
    running,
    completed,
    failed,
    cancelled,
};

struct TransferProgress {
    std::uint64_t transfer_id = 0;
    TransferState state = TransferState::running;
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
    double elapsed_seconds = 0.0;
};

struct TransferStarted {
    std::uint64_t transfer_id = 0;
    TransferState state = TransferState::running;
    TransferDirection direction = TransferDirection::send;
    TransferKind kind = TransferKind::file;
    std::filesystem::path path;
    std::string name;
};

struct TransferCompleted {
    std::uint64_t transfer_id = 0;
    TransferState state = TransferState::completed;
    TransferDirection direction = TransferDirection::send;
    TransferKind kind = TransferKind::file;
    std::filesystem::path path;
    std::string name;
    std::uint64_t bytes = 0;
    std::uint64_t total_files = 0;
    std::uint64_t skipped_files = 0;
    std::uint64_t full_files = 0;
    std::uint64_t delta_files = 0;
    std::uint64_t payload_bytes = 0;
    TransferCompletionStatus status = TransferCompletionStatus::transferred;
    std::uint64_t resumed_from = 0;
    double elapsed_seconds = 0.0;
    FileTransferSource source = FileTransferSource::file;
};

struct TransferFailed {
    std::uint64_t transfer_id = 0;
    TransferState state = TransferState::failed;
    TransferDirection direction = TransferDirection::send;
    TransferKind kind = TransferKind::file;
    std::filesystem::path path;
    std::string name;
    Error error;
    ErrorCategory category = ErrorCategory::internal;
    bool retryable = false;
    bool user_action_required = false;
};

struct TransferCancelled {
    std::uint64_t transfer_id = 0;
    TransferState state = TransferState::cancelled;
    TransferDirection direction = TransferDirection::send;
    TransferKind kind = TransferKind::file;
    std::filesystem::path path;
    std::string name;
};

std::string_view transfer_state_name(TransferState state);
bool can_transition(TransferState from, TransferState to);

class TransferEvents {
public:
    virtual ~TransferEvents() = default;

    virtual void on_transfer_started(const TransferStarted& started);
    virtual void on_transfer_progress(const TransferProgress& progress);
    virtual void on_transfer_completed(const TransferCompleted& completed);
    virtual void on_transfer_failed(const TransferFailed& failed);
    virtual void on_transfer_cancelled(const TransferCancelled& cancelled);
};

}  // namespace lan
