#pragma once

#include <filesystem>

#include "lan/app/receiver_config.h"
#include "lan/app/sender_config.h"
#include "lan/common/result.h"
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

Result<SendSyncNegotiationReport> negotiate_sync_sender(const SenderConfig& config,
                                                        std::uint32_t block_size);
Result<ReceiveSyncNegotiationReport> negotiate_sync_receiver(const ReceiverConfig& config,
                                                             std::uint32_t block_size);

}  // namespace lan
