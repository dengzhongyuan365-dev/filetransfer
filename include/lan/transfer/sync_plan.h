#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "lan/common/result.h"
#include "lan/transfer/block_signature.h"
#include "lan/transfer/manifest.h"

namespace lan {

enum class SyncAction {
    skip,
    full,
    delta,
};

struct SyncPlanEntry {
    ManifestEntry manifest_entry;
    SyncAction action = SyncAction::full;
    std::vector<BlockSignature> basis_signatures;
};

struct SyncPlan {
    std::filesystem::path receive_root;
    std::uint32_t block_size = 0;
    std::vector<SyncPlanEntry> entries;
};

Result<SyncPlan> build_sync_plan(const Manifest& manifest,
                                 const std::filesystem::path& receive_root,
                                 std::uint32_t block_size);

}  // namespace lan
