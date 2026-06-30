#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

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

class TransferEvents {
public:
    virtual ~TransferEvents() = default;

    virtual void on_transfer_progress(const TransferProgress& progress);
};

}  // namespace lan
