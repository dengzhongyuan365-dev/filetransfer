#include "lan/transfer/delta.h"

#include <deque>
#include <fstream>
#include <limits>
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

using SignatureLookup = std::unordered_multimap<std::uint32_t, const BlockSignature*>;

SignatureLookup build_lookup(const std::vector<BlockSignature>& signatures) {
    SignatureLookup lookup;
    for (const auto& signature : signatures) {
        lookup.emplace(signature.weak_checksum, &signature);
    }
    return lookup;
}

std::vector<std::byte> copy_window(const std::deque<std::byte>& window) {
    return std::vector<std::byte>(window.begin(), window.end());
}

Result<const BlockSignature*> find_match(const SignatureLookup& lookup,
                                         std::uint32_t weak,
                                         const std::deque<std::byte>& window) {
    const auto range = lookup.equal_range(weak);
    std::string strong;
    bool strong_computed = false;

    for (auto it = range.first; it != range.second; ++it) {
        const auto* signature = it->second;
        if (signature->size != window.size()) {
            continue;
        }

        if (!strong_computed) {
            const auto bytes = copy_window(window);
            auto digest = sha256_bytes(bytes);
            if (!digest) {
                return Result<const BlockSignature*>::failure(digest.error());
            }
            strong = std::move(digest).value();
            strong_computed = true;
        }

        if (strong == signature->strong_checksum) {
            return Result<const BlockSignature*>::success(signature);
        }
    }

    return Result<const BlockSignature*>::success(nullptr);
}

class RollingChecksum {
public:
    static RollingChecksum from_window(const std::deque<std::byte>& window) {
        RollingChecksum checksum;
        checksum.size_ = static_cast<std::uint32_t>(window.size());

        for (std::uint32_t i = 0; i < checksum.size_; ++i) {
            const auto value = byte_value(window[i]);
            checksum.a_ = (checksum.a_ + value) & 0xffffu;
            checksum.b_ = (checksum.b_ + (checksum.size_ - i) * value) & 0xffffu;
        }

        return checksum;
    }

    std::uint32_t value() const {
        return a_ | (b_ << 16);
    }

    void slide(std::byte old_byte, std::byte new_byte) {
        const auto old_value = byte_value(old_byte);
        const auto new_value = byte_value(new_byte);
        a_ = (a_ + 0x10000u - old_value + new_value) & 0xffffu;
        b_ = (b_ + 0x10000u - ((size_ * old_value) & 0xffffu) + a_) & 0xffffu;
    }

private:
    static std::uint32_t byte_value(std::byte value) {
        return std::to_integer<std::uint32_t>(value);
    }

    std::uint32_t size_ = 0;
    std::uint32_t a_ = 0;
    std::uint32_t b_ = 0;
};

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

Result<bool> read_byte(std::ifstream& input, std::byte& output) {
    char value = 0;
    if (input.get(value)) {
        output = static_cast<std::byte>(static_cast<unsigned char>(value));
        return Result<bool>::success(true);
    }

    if (input.eof()) {
        return Result<bool>::success(false);
    }

    return Result<bool>::failure(make_error(ErrorCode::io_error, "failed to read delta source"));
}

Result<bool> fill_window(std::ifstream& input,
                         std::deque<std::byte>& window,
                         std::uint32_t block_size) {
    while (window.size() < block_size) {
        std::byte next{};
        auto byte = read_byte(input, next);
        if (!byte) {
            return byte;
        }
        if (!byte.value()) {
            break;
        }
        window.push_back(next);
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

    std::error_code ec;
    const auto source_size = std::filesystem::file_size(source_path, ec);
    if (ec) {
        return Result<DeltaPlan>::failure(
            make_error(ErrorCode::io_error,
                       "failed to read size of " + quote_path(source_path) + ": " + ec.message()));
    }

    auto source_hash = hash_file(source_path);
    if (!source_hash) {
        return Result<DeltaPlan>::failure(source_hash.error());
    }

    DeltaPlan plan;
    plan.source_size = source_size;
    plan.source_sha256 = source_hash.value().hex_digest;

    auto op_count = stream_delta_ops(
        source_path,
        basis_signatures,
        block_size,
        [&](const DeltaOp& op) {
            plan.ops.push_back(op);
            return Result<bool>::success(true);
        });
    if (!op_count) {
        return Result<DeltaPlan>::failure(op_count.error());
    }

    plan.op_count = op_count.value();
    return Result<DeltaPlan>::success(std::move(plan));
}

Result<std::uint32_t> stream_delta_ops(const std::filesystem::path& source_path,
                                       const std::vector<BlockSignature>& basis_signatures,
                                       std::uint32_t block_size,
                                       const DeltaOpCallback& on_op) {
    if (block_size == 0) {
        return Result<std::uint32_t>::failure(
            make_error(ErrorCode::invalid_argument, "block size must be greater than zero"));
    }
    if (!on_op) {
        return Result<std::uint32_t>::failure(
            make_error(ErrorCode::invalid_argument, "delta op callback is required"));
    }

    std::ifstream input(source_path, std::ios::binary);
    if (!input) {
        return Result<std::uint32_t>::failure(
            make_error(ErrorCode::io_error, "failed to open " + quote_path(source_path)));
    }

    const auto lookup = build_lookup(basis_signatures);
    std::deque<std::byte> window;
    std::vector<std::byte> literal;
    std::uint32_t op_count = 0;

    auto emit_op = [&](const DeltaOp& op) -> Result<bool> {
        if (op_count == std::numeric_limits<std::uint32_t>::max()) {
            return Result<bool>::failure(
                make_error(ErrorCode::invalid_argument, "delta stream has too many operations"));
        }

        auto emitted = on_op(op);
        if (!emitted) {
            return emitted;
        }
        if (!emitted.value()) {
            return Result<bool>::failure(
                make_error(ErrorCode::internal_error, "delta op callback rejected op"));
        }
        ++op_count;
        return Result<bool>::success(true);
    };

    auto flush_stream_literal = [&]() -> Result<bool> {
        if (literal.empty()) {
            return Result<bool>::success(true);
        }

        DeltaOp op;
        op.type = DeltaOpType::literal_data;
        op.size = static_cast<std::uint64_t>(literal.size());
        op.data = std::move(literal);
        literal.clear();
        return emit_op(op);
    };

    auto filled = fill_window(input, window, block_size);
    if (!filled) {
        return Result<std::uint32_t>::failure(filled.error());
    }

    while (window.size() == block_size) {
        auto checksum = RollingChecksum::from_window(window);

        while (window.size() == block_size) {
            auto match = find_match(lookup, checksum.value(), window);
            if (!match) {
                return Result<std::uint32_t>::failure(match.error());
            }

            if (match.value() != nullptr) {
                auto flushed = flush_stream_literal();
                if (!flushed) {
                    return Result<std::uint32_t>::failure(flushed.error());
                }

                DeltaOp op;
                op.type = DeltaOpType::copy_from_basis;
                op.basis_offset = match.value()->offset;
                op.size = match.value()->size;
                auto emitted = emit_op(op);
                if (!emitted) {
                    return Result<std::uint32_t>::failure(emitted.error());
                }

                window.clear();
                filled = fill_window(input, window, block_size);
                if (!filled) {
                    return Result<std::uint32_t>::failure(filled.error());
                }
                break;
            }

            const auto old_byte = window.front();
            literal.push_back(old_byte);
            if (literal.size() >= block_size) {
                auto flushed = flush_stream_literal();
                if (!flushed) {
                    return Result<std::uint32_t>::failure(flushed.error());
                }
            }
            window.pop_front();

            std::byte next{};
            auto byte = read_byte(input, next);
            if (!byte) {
                return Result<std::uint32_t>::failure(
                    make_error(byte.error().code, byte.error().message + ": " + quote_path(source_path)));
            }
            if (!byte.value()) {
                break;
            }

            window.push_back(next);
            checksum.slide(old_byte, next);
        }
    }

    if (!window.empty()) {
        const auto bytes = copy_window(window);
        auto match = find_match(lookup, weak_checksum(bytes), window);
        if (!match) {
            return Result<std::uint32_t>::failure(match.error());
        }

        if (match.value() != nullptr) {
            auto flushed = flush_stream_literal();
            if (!flushed) {
                return Result<std::uint32_t>::failure(flushed.error());
            }

            DeltaOp op;
            op.type = DeltaOpType::copy_from_basis;
            op.basis_offset = match.value()->offset;
            op.size = match.value()->size;
            auto emitted = emit_op(op);
            if (!emitted) {
                return Result<std::uint32_t>::failure(emitted.error());
            }
        } else {
            literal.insert(literal.end(), window.begin(), window.end());
            if (literal.size() >= block_size) {
                auto flushed = flush_stream_literal();
                if (!flushed) {
                    return Result<std::uint32_t>::failure(flushed.error());
                }
            }
        }
    }

    auto flushed = flush_stream_literal();
    if (!flushed) {
        return Result<std::uint32_t>::failure(flushed.error());
    }

    return Result<std::uint32_t>::success(op_count);
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
