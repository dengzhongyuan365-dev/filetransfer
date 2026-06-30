#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "lan/common/result.h"
#include "lan/transfer/block_signature.h"

namespace lan {

enum class DeltaOpType {
    copy_from_basis,
    literal_data,
};

struct DeltaOp {
    DeltaOpType type = DeltaOpType::literal_data;
    std::uint64_t basis_offset = 0;
    std::uint64_t size = 0;
    std::vector<std::byte> data;
};

struct DeltaPlan {
    std::uint64_t source_size = 0;
    std::string source_sha256;
    std::vector<DeltaOp> ops;
};

Result<DeltaPlan> build_delta(const std::filesystem::path& source_path,
                              const std::vector<BlockSignature>& basis_signatures,
                              std::uint32_t block_size);
Result<bool> apply_delta(const std::filesystem::path& basis_path,
                         const std::filesystem::path& output_path,
                         const std::vector<DeltaOp>& ops);

}  // namespace lan
