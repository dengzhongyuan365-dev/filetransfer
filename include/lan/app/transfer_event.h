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

struct TransferProgress {
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
    TransferDirection direction = TransferDirection::send;
    TransferKind kind = TransferKind::file;
    std::filesystem::path path;
    std::string name;
};

struct TransferCompleted {
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
    double elapsed_seconds = 0.0;
};

struct TransferFailed {
    TransferDirection direction = TransferDirection::send;
    TransferKind kind = TransferKind::file;
    std::filesystem::path path;
    std::string name;
    Error error;
};

struct TransferCancelled {
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
