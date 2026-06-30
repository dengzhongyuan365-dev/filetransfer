#pragma once

#include <cstdint>
#include <filesystem>

#include "lan/common/result.h"
#include "lan/transfer/manifest.h"
#include "lan/transfer/sync_plan.h"

namespace lan {

struct SyncReport {
    std::uint64_t skipped_files = 0;
    std::uint64_t full_files = 0;
    std::uint64_t delta_files = 0;
    std::uint64_t bytes_written = 0;
};

Result<SyncReport> execute_local_sync(const Manifest& source_manifest,
                                      const std::filesystem::path& source_root,
                                      const SyncPlan& plan);

}  // namespace lan
