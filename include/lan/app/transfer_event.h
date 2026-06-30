#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "lan/common/error.h"

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

struct TransferProgress {
    std::uint64_t transfer_id = 0;
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
    TransferDirection direction = TransferDirection::send;
    TransferKind kind = TransferKind::file;
    std::filesystem::path path;
    std::string name;
};

struct TransferCompleted {
    std::uint64_t transfer_id = 0;
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
};

struct TransferFailed {
    std::uint64_t transfer_id = 0;
    TransferDirection direction = TransferDirection::send;
    TransferKind kind = TransferKind::file;
    std::filesystem::path path;
    std::string name;
    Error error;
};

struct TransferCancelled {
    std::uint64_t transfer_id = 0;
    TransferDirection direction = TransferDirection::send;
    TransferKind kind = TransferKind::file;
    std::filesystem::path path;
    std::string name;
};

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
