#include "lan/transfer/delta.h"

#include <algorithm>
#include <fstream>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

#include "lan/fs/file_hash.h"

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

std::string quote_path(const std::filesystem::path& path) {
    return "'" + path.string() + "'";
}

Result<std::vector<std::byte>> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<std::vector<std::byte>>::failure(
            make_error(ErrorCode::io_error, "failed to open " + quote_path(path)));
    }

    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0) {
        return Result<std::vector<std::byte>>::failure(
            make_error(ErrorCode::io_error, "failed to read size of " + quote_path(path)));
    }

    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            return Result<std::vector<std::byte>>::failure(
                make_error(ErrorCode::io_error, "failed to read " + quote_path(path)));
        }
    }

    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

using SignatureLookup = std::unordered_multimap<std::uint32_t, const BlockSignature*>;

SignatureLookup build_lookup(const std::vector<BlockSignature>& signatures) {
    SignatureLookup lookup;
    for (const auto& signature : signatures) {
        lookup.emplace(signature.weak_checksum, &signature);
    }
    return lookup;
}

const BlockSignature* find_match(const SignatureLookup& lookup,
                                 std::uint32_t weak,
                                 std::span<const std::byte> window) {
    const auto range = lookup.equal_range(weak);
    for (auto it = range.first; it != range.second; ++it) {
        const auto* signature = it->second;
        if (signature->size != window.size()) {
            continue;
        }

        auto strong = sha256_bytes(window);
        if (strong && strong.value() == signature->strong_checksum) {
            return signature;
        }
    }

    return nullptr;
}

void flush_literal(std::vector<DeltaOp>& ops, std::vector<std::byte>& literal) {
    if (literal.empty()) {
        return;
    }

    ops.push_back(DeltaOp{
        .type = DeltaOpType::literal_data,
        .basis_offset = 0,
        .size = static_cast<std::uint64_t>(literal.size()),
        .data = std::move(literal),
    });
    literal.clear();
}

Result<bool> write_bytes(std::ofstream& output, const std::byte* data, std::size_t size) {
    if (size == 0) {
        return Result<bool>::success(true);
    }

    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!output) {
        return Result<bool>::failure(make_error(ErrorCode::io_error, "failed to write delta output"));
    }

    return Result<bool>::success(true);
}

}  // namespace

Result<DeltaPlan> build_delta(const std::filesystem::path& source_path,
                              const std::vector<BlockSignature>& basis_signatures,
                              std::uint32_t block_size) {
    if (block_size == 0) {
        return Result<DeltaPlan>::failure(
            make_error(ErrorCode::invalid_argument, "block size must be greater than zero"));
    }

    auto source = read_file_bytes(source_path);
    if (!source) {
        return Result<DeltaPlan>::failure(source.error());
    }

    auto source_hash = hash_file(source_path);
    if (!source_hash) {
        return Result<DeltaPlan>::failure(source_hash.error());
    }

    DeltaPlan plan;
    plan.source_size = source.value().size();
    plan.source_sha256 = source_hash.value().hex_digest;

    if (source.value().empty()) {
        return Result<DeltaPlan>::success(std::move(plan));
    }

    const auto lookup = build_lookup(basis_signatures);
    std::vector<std::byte> literal;
    std::size_t position = 0;

    while (position < source.value().size()) {
        const auto remaining = source.value().size() - position;
        const auto window_size = std::min<std::size_t>(block_size, remaining);
        const auto window = std::span<const std::byte>(source.value().data() + position, window_size);
        const auto weak = weak_checksum(window);

        if (const auto* match = find_match(lookup, weak, window)) {
            flush_literal(plan.ops, literal);
            plan.ops.push_back(DeltaOp{
                .type = DeltaOpType::copy_from_basis,
                .basis_offset = match->offset,
                .size = match->size,
                .data = {},
            });
            position += match->size;
            continue;
        }

        literal.push_back(source.value()[position]);
        ++position;
    }

    flush_literal(plan.ops, literal);
    plan.op_count = static_cast<std::uint32_t>(plan.ops.size());
    return Result<DeltaPlan>::success(std::move(plan));
}

Result<bool> apply_delta(const std::filesystem::path& basis_path,
                         const std::filesystem::path& output_path,
                         const std::vector<DeltaOp>& ops) {
    std::ifstream basis(basis_path, std::ios::binary);
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Result<bool>::failure(
            make_error(ErrorCode::io_error, "failed to open delta output " + quote_path(output_path)));
    }

    for (const auto& op : ops) {
        auto applied = apply_delta_op(basis, output, op);
        if (!applied) {
            return applied;
        }
    }

    return Result<bool>::success(true);
}

Result<bool> apply_delta_op(std::ifstream& basis,
                            std::ofstream& output,
                            const DeltaOp& op) {
    if (op.type == DeltaOpType::literal_data) {
        return write_bytes(output, op.data.data(), op.data.size());
    }

    if (!basis) {
        return Result<bool>::failure(
            make_error(ErrorCode::io_error, "failed to open delta basis"));
    }

    basis.clear();
    basis.seekg(static_cast<std::streamoff>(op.basis_offset), std::ios::beg);
    if (!basis) {
        return Result<bool>::failure(make_error(ErrorCode::io_error, "failed to seek delta basis"));
    }

    std::vector<std::byte> copy_buffer(static_cast<std::size_t>(op.size));
    if (!copy_buffer.empty()) {
        basis.read(reinterpret_cast<char*>(copy_buffer.data()),
                   static_cast<std::streamsize>(copy_buffer.size()));
        if (!basis) {
            return Result<bool>::failure(make_error(ErrorCode::io_error, "failed to read delta basis"));
        }
    }

    return write_bytes(output, copy_buffer.data(), copy_buffer.size());
}

}  // namespace lan
