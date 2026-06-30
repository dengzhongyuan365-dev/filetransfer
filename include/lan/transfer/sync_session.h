#pragma once

#include <functional>
#include <filesystem>

#include "lan/app/receiver_config.h"
#include "lan/app/sender_config.h"
#include "lan/common/result.h"
#include "lan/net/connection.h"
#include "lan/protocol/frame.h"
#include "lan/transfer/sync_plan.h"

namespace lan {

struct SendSyncNegotiationReport {
    std::uint64_t manifest_files = 0;
    std::uint64_t skip_files = 0;
    std::uint64_t full_files = 0;
    std::uint64_t delta_files = 0;
};

struct ReceiveSyncNegotiationReport {
    std::uint64_t manifest_files = 0;
    std::uint64_t skip_files = 0;
    std::uint64_t full_files = 0;
    std::uint64_t delta_files = 0;
};

struct SendSyncReport {
    std::uint64_t manifest_files = 0;
    std::uint64_t skipped_files = 0;
    std::uint64_t full_files = 0;
    std::uint64_t delta_files = 0;
    std::uint64_t delta_frames_sent = 0;
    std::uint32_t block_size = 0;
    double elapsed_seconds = 0.0;
};

struct SendSyncProgress {
    std::uint64_t manifest_files = 0;
    std::uint64_t processed_files = 0;
    std::uint64_t skipped_files = 0;
    std::uint64_t full_files = 0;
    std::uint64_t delta_files = 0;
    std::uint64_t delta_frames_sent = 0;
    std::uint32_t block_size = 0;
    double elapsed_seconds = 0.0;
};

struct ReceiveSyncReport {
    std::uint64_t manifest_files = 0;
    std::uint64_t skipped_files = 0;
    std::uint64_t full_files = 0;
    std::uint64_t delta_files = 0;
    std::uint64_t files_written = 0;
    std::uint32_t block_size = 0;
    double elapsed_seconds = 0.0;
};

struct ReceiveSyncProgress {
    std::uint64_t manifest_files = 0;
    std::uint64_t processed_files = 0;
    std::uint64_t skipped_files = 0;
    std::uint64_t full_files = 0;
    std::uint64_t delta_files = 0;
    std::uint64_t files_written = 0;
    std::uint32_t block_size = 0;
    double elapsed_seconds = 0.0;
};

using ReceiveSyncProgressCallback = std::function<void(const ReceiveSyncProgress&)>;
using SendSyncProgressCallback = std::function<void(const SendSyncProgress&)>;

Result<SendSyncNegotiationReport> negotiate_sync_sender(const SenderConfig& config,
                                                        std::uint32_t block_size);
Result<ReceiveSyncNegotiationReport> negotiate_sync_receiver(const ReceiverConfig& config,
                                                             std::uint32_t block_size);
Result<SendSyncReport> sync_sender_to_connection(const SenderConfig& config,
                                                 std::uint32_t block_size,
                                                 Connection& connection);
Result<SendSyncReport> sync_sender_to_connection(const SenderConfig& config,
                                                 std::uint32_t block_size,
                                                 Connection& connection,
                                                 SendSyncProgressCallback on_progress);
Result<SendSyncReport> sync_sender(const SenderConfig& config, std::uint32_t block_size);
Result<SendSyncReport> sync_sender(const SenderConfig& config,
                                   std::uint32_t block_size,
                                   SendSyncProgressCallback on_progress);
Result<ReceiveSyncReport> sync_receiver(const ReceiverConfig& config, std::uint32_t block_size);
Result<ReceiveSyncReport> sync_receiver_from_connection(const ReceiverConfig& config,
                                                        std::uint32_t block_size,
                                                        Connection& connection,
                                                        const Frame& hello);
Result<ReceiveSyncReport> sync_receiver_from_connection(
    const ReceiverConfig& config,
    std::uint32_t block_size,
    Connection& connection,
    const Frame& hello,
    ReceiveSyncProgressCallback on_progress);

}  // namespace lan
