#pragma once

#include <cstddef>
#include <vector>

#include "lan/common/result.h"
#include "lan/transfer/delta.h"
#include "lan/transfer/manifest.h"
#include "lan/transfer/sync_plan.h"

namespace lan {

std::vector<std::byte> encode_manifest(const Manifest& manifest);
Result<Manifest> decode_manifest(const std::vector<std::byte>& body);

std::vector<std::byte> encode_sync_plan(const SyncPlan& plan);
Result<SyncPlan> decode_sync_plan(const std::vector<std::byte>& body);

std::vector<std::byte> encode_delta_plan(const DeltaPlan& plan);
Result<DeltaPlan> decode_delta_plan(const std::vector<std::byte>& body);

}  // namespace lan
