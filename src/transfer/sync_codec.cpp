#include "lan/transfer/sync_codec.h"

#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

void append_u8(std::vector<std::byte>& output, std::uint8_t value) {
    output.push_back(static_cast<std::byte>(value));
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (int i = 3; i >= 0; --i) {
        output.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xff));
    }
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        output.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xff));
    }
}

void append_bytes(std::vector<std::byte>& output, const std::byte* data, std::size_t size) {
    output.insert(output.end(), data, data + size);
}

void append_string(std::vector<std::byte>& output, std::string_view value) {
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    append_bytes(output, reinterpret_cast<const std::byte*>(value.data()), value.size());
}

class Reader {
public:
    explicit Reader(const std::vector<std::byte>& body) : body_(body) {}

    Result<std::uint8_t> read_u8() {
        if (remaining() < 1) {
            return failure<std::uint8_t>("unexpected end while reading u8");
        }

        return Result<std::uint8_t>::success(std::to_integer<std::uint8_t>(body_[offset_++]));
    }

    Result<std::uint32_t> read_u32() {
        if (remaining() < 4) {
            return failure<std::uint32_t>("unexpected end while reading u32");
        }

        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value = (value << 8) | std::to_integer<std::uint32_t>(body_[offset_++]);
        }
        return Result<std::uint32_t>::success(value);
    }

    Result<std::uint64_t> read_u64() {
        if (remaining() < 8) {
            return failure<std::uint64_t>("unexpected end while reading u64");
        }

        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value = (value << 8) | std::to_integer<std::uint64_t>(body_[offset_++]);
        }
        return Result<std::uint64_t>::success(value);
    }

    Result<std::string> read_string() {
        auto size = read_u32();
        if (!size) {
            return Result<std::string>::failure(size.error());
        }

        if (remaining() < size.value()) {
            return failure<std::string>("unexpected end while reading string");
        }

        std::string value(size.value(), '\0');
        std::memcpy(value.data(), body_.data() + offset_, size.value());
        offset_ += size.value();
        return Result<std::string>::success(std::move(value));
    }

    Result<std::vector<std::byte>> read_bytes() {
        auto size = read_u64();
        if (!size) {
            return Result<std::vector<std::byte>>::failure(size.error());
        }

        if (remaining() < size.value()) {
            return failure<std::vector<std::byte>>("unexpected end while reading bytes");
        }

        std::vector<std::byte> value(size.value());
        std::memcpy(value.data(), body_.data() + offset_, static_cast<std::size_t>(size.value()));
        offset_ += static_cast<std::size_t>(size.value());
        return Result<std::vector<std::byte>>::success(std::move(value));
    }

    bool done() const {
        return offset_ == body_.size();
    }

private:
    std::size_t remaining() const {
        return body_.size() - offset_;
    }

    template <typename T>
    Result<T> failure(std::string message) const {
        return Result<T>::failure(make_error(ErrorCode::protocol_error, std::move(message)));
    }

    const std::vector<std::byte>& body_;
    std::size_t offset_ = 0;
};

bool is_safe_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return false;
    }

    for (const auto& part : path) {
        if (part == "." || part == "..") {
            return false;
        }
    }

    return true;
}

void append_manifest_entry(std::vector<std::byte>& output, const ManifestEntry& entry) {
    append_string(output, entry.relative_path.generic_string());
    append_u64(output, entry.size);
    append_string(output, entry.sha256);
    append_u64(output, entry.mtime_ns);
    append_u32(output, entry.mode);
}

Result<ManifestEntry> read_manifest_entry(Reader& reader) {
    auto path = reader.read_string();
    if (!path) {
        return Result<ManifestEntry>::failure(path.error());
    }
    auto size = reader.read_u64();
    if (!size) {
        return Result<ManifestEntry>::failure(size.error());
    }
    auto sha256 = reader.read_string();
    if (!sha256) {
        return Result<ManifestEntry>::failure(sha256.error());
    }
    auto mtime = reader.read_u64();
    if (!mtime) {
        return Result<ManifestEntry>::failure(mtime.error());
    }
    auto mode = reader.read_u32();
    if (!mode) {
        return Result<ManifestEntry>::failure(mode.error());
    }

    std::filesystem::path relative(path.value());
    if (!is_safe_relative_path(relative)) {
        return Result<ManifestEntry>::failure(
            make_error(ErrorCode::protocol_error, "unsafe manifest relative path"));
    }

    return Result<ManifestEntry>::success(ManifestEntry{
        .relative_path = relative.lexically_normal(),
        .size = size.value(),
        .sha256 = sha256.value(),
        .mtime_ns = mtime.value(),
        .mode = mode.value(),
    });
}

void append_signature(std::vector<std::byte>& output, const BlockSignature& signature) {
    append_u64(output, signature.index);
    append_u64(output, signature.offset);
    append_u32(output, signature.size);
    append_u32(output, signature.weak_checksum);
    append_string(output, signature.strong_checksum);
}

Result<BlockSignature> read_signature(Reader& reader) {
    auto index = reader.read_u64();
    if (!index) {
        return Result<BlockSignature>::failure(index.error());
    }
    auto offset = reader.read_u64();
    if (!offset) {
        return Result<BlockSignature>::failure(offset.error());
    }
    auto size = reader.read_u32();
    if (!size) {
        return Result<BlockSignature>::failure(size.error());
    }
    auto weak = reader.read_u32();
    if (!weak) {
        return Result<BlockSignature>::failure(weak.error());
    }
    auto strong = reader.read_string();
    if (!strong) {
        return Result<BlockSignature>::failure(strong.error());
    }

    return Result<BlockSignature>::success(BlockSignature{
        .index = index.value(),
        .offset = offset.value(),
        .size = size.value(),
        .weak_checksum = weak.value(),
        .strong_checksum = strong.value(),
    });
}

Result<bool> require_done(const Reader& reader) {
    if (!reader.done()) {
        return Result<bool>::failure(
            make_error(ErrorCode::protocol_error, "trailing bytes in sync codec body"));
    }

    return Result<bool>::success(true);
}

}  // namespace

std::vector<std::byte> encode_manifest(const Manifest& manifest) {
    std::vector<std::byte> output;
    append_u32(output, static_cast<std::uint32_t>(manifest.files.size()));
    for (const auto& entry : manifest.files) {
        append_manifest_entry(output, entry);
    }
    return output;
}

Result<Manifest> decode_manifest(const std::vector<std::byte>& body) {
    Reader reader(body);
    auto count = reader.read_u32();
    if (!count) {
        return Result<Manifest>::failure(count.error());
    }

    Manifest manifest;
    manifest.files.reserve(count.value());
    for (std::uint32_t i = 0; i < count.value(); ++i) {
        auto entry = read_manifest_entry(reader);
        if (!entry) {
            return Result<Manifest>::failure(entry.error());
        }
        manifest.files.push_back(std::move(entry).value());
    }

    auto done = require_done(reader);
    if (!done) {
        return Result<Manifest>::failure(done.error());
    }

    return Result<Manifest>::success(std::move(manifest));
}

std::vector<std::byte> encode_sync_plan(const SyncPlan& plan) {
    std::vector<std::byte> output;
    append_u32(output, plan.block_size);
    append_u32(output, static_cast<std::uint32_t>(plan.entries.size()));

    for (const auto& entry : plan.entries) {
        append_u8(output, static_cast<std::uint8_t>(entry.action));
        append_manifest_entry(output, entry.manifest_entry);
        append_u32(output, static_cast<std::uint32_t>(entry.basis_signatures.size()));
        for (const auto& signature : entry.basis_signatures) {
            append_signature(output, signature);
        }
    }

    return output;
}

Result<SyncPlan> decode_sync_plan(const std::vector<std::byte>& body) {
    Reader reader(body);
    auto block_size = reader.read_u32();
    if (!block_size) {
        return Result<SyncPlan>::failure(block_size.error());
    }
    auto count = reader.read_u32();
    if (!count) {
        return Result<SyncPlan>::failure(count.error());
    }

    SyncPlan plan;
    plan.block_size = block_size.value();
    plan.entries.reserve(count.value());

    for (std::uint32_t i = 0; i < count.value(); ++i) {
        auto raw_action = reader.read_u8();
        if (!raw_action) {
            return Result<SyncPlan>::failure(raw_action.error());
        }
        if (raw_action.value() > static_cast<std::uint8_t>(SyncAction::delta)) {
            return Result<SyncPlan>::failure(
                make_error(ErrorCode::protocol_error, "invalid sync action"));
        }

        auto manifest_entry = read_manifest_entry(reader);
        if (!manifest_entry) {
            return Result<SyncPlan>::failure(manifest_entry.error());
        }
        auto signature_count = reader.read_u32();
        if (!signature_count) {
            return Result<SyncPlan>::failure(signature_count.error());
        }

        SyncPlanEntry entry;
        entry.action = static_cast<SyncAction>(raw_action.value());
        entry.manifest_entry = std::move(manifest_entry).value();
        entry.basis_signatures.reserve(signature_count.value());

        for (std::uint32_t j = 0; j < signature_count.value(); ++j) {
            auto signature = read_signature(reader);
            if (!signature) {
                return Result<SyncPlan>::failure(signature.error());
            }
            entry.basis_signatures.push_back(std::move(signature).value());
        }

        plan.entries.push_back(std::move(entry));
    }

    auto done = require_done(reader);
    if (!done) {
        return Result<SyncPlan>::failure(done.error());
    }

    return Result<SyncPlan>::success(std::move(plan));
}

std::vector<std::byte> encode_delta_plan(const DeltaPlan& plan) {
    std::vector<std::byte> output;
    append_u64(output, plan.source_size);
    append_string(output, plan.source_sha256);
    append_u32(output, plan.op_count == 0 ? static_cast<std::uint32_t>(plan.ops.size()) : plan.op_count);

    for (const auto& op : plan.ops) {
        append_u8(output, static_cast<std::uint8_t>(op.type));
        append_u64(output, op.basis_offset);
        append_u64(output, op.size);
        append_u64(output, op.data.size());
        append_bytes(output, op.data.data(), op.data.size());
    }

    return output;
}

Result<DeltaPlan> decode_delta_plan(const std::vector<std::byte>& body) {
    Reader reader(body);
    auto source_size = reader.read_u64();
    if (!source_size) {
        return Result<DeltaPlan>::failure(source_size.error());
    }
    auto source_sha256 = reader.read_string();
    if (!source_sha256) {
        return Result<DeltaPlan>::failure(source_sha256.error());
    }
    auto op_count = reader.read_u32();
    if (!op_count) {
        return Result<DeltaPlan>::failure(op_count.error());
    }

    DeltaPlan plan;
    plan.source_size = source_size.value();
    plan.source_sha256 = source_sha256.value();
    plan.op_count = op_count.value();
    plan.ops.reserve(op_count.value());

    for (std::uint32_t i = 0; i < op_count.value(); ++i) {
        auto raw_type = reader.read_u8();
        if (!raw_type) {
            return Result<DeltaPlan>::failure(raw_type.error());
        }
        if (raw_type.value() > static_cast<std::uint8_t>(DeltaOpType::literal_data)) {
            return Result<DeltaPlan>::failure(
                make_error(ErrorCode::protocol_error, "invalid delta op type"));
        }
        auto basis_offset = reader.read_u64();
        if (!basis_offset) {
            return Result<DeltaPlan>::failure(basis_offset.error());
        }
        auto size = reader.read_u64();
        if (!size) {
            return Result<DeltaPlan>::failure(size.error());
        }
        auto data = reader.read_bytes();
        if (!data) {
            return Result<DeltaPlan>::failure(data.error());
        }

        plan.ops.push_back(DeltaOp{
            .type = static_cast<DeltaOpType>(raw_type.value()),
            .basis_offset = basis_offset.value(),
            .size = size.value(),
            .data = std::move(data).value(),
        });
    }

    auto done = require_done(reader);
    if (!done) {
        return Result<DeltaPlan>::failure(done.error());
    }

    return Result<DeltaPlan>::success(std::move(plan));
}

std::vector<std::byte> encode_delta_header(const DeltaPlan& plan) {
    std::vector<std::byte> output;
    append_u64(output, plan.source_size);
    append_string(output, plan.source_sha256);
    return output;
}

Result<DeltaPlan> decode_delta_header(const std::vector<std::byte>& body) {
    Reader reader(body);
    auto source_size = reader.read_u64();
    if (!source_size) {
        return Result<DeltaPlan>::failure(source_size.error());
    }
    auto source_sha256 = reader.read_string();
    if (!source_sha256) {
        return Result<DeltaPlan>::failure(source_sha256.error());
    }

    auto done = require_done(reader);
    if (!done) {
        return Result<DeltaPlan>::failure(done.error());
    }

    DeltaPlan plan;
    plan.source_size = source_size.value();
    plan.source_sha256 = source_sha256.value();
    return Result<DeltaPlan>::success(std::move(plan));
}

std::vector<std::byte> encode_delta_end(const DeltaPlan& plan) {
    std::vector<std::byte> output;
    append_u32(output, plan.op_count == 0 ? static_cast<std::uint32_t>(plan.ops.size()) : plan.op_count);
    return output;
}

Result<DeltaPlan> decode_delta_end(const std::vector<std::byte>& body) {
    Reader reader(body);
    auto op_count = reader.read_u32();
    if (!op_count) {
        return Result<DeltaPlan>::failure(op_count.error());
    }

    auto done = require_done(reader);
    if (!done) {
        return Result<DeltaPlan>::failure(done.error());
    }

    DeltaPlan plan;
    plan.op_count = op_count.value();
    return Result<DeltaPlan>::success(plan);
}

std::vector<std::byte> encode_delta_op(const DeltaOp& op) {
    std::vector<std::byte> output;
    append_u8(output, static_cast<std::uint8_t>(op.type));
    append_u64(output, op.basis_offset);
    append_u64(output, op.size);
    append_u64(output, op.data.size());
    append_bytes(output, op.data.data(), op.data.size());
    return output;
}

Result<DeltaOp> decode_delta_op(const std::vector<std::byte>& body) {
    Reader reader(body);
    auto raw_type = reader.read_u8();
    if (!raw_type) {
        return Result<DeltaOp>::failure(raw_type.error());
    }
    if (raw_type.value() > static_cast<std::uint8_t>(DeltaOpType::literal_data)) {
        return Result<DeltaOp>::failure(
            make_error(ErrorCode::protocol_error, "invalid delta op type"));
    }
    auto basis_offset = reader.read_u64();
    if (!basis_offset) {
        return Result<DeltaOp>::failure(basis_offset.error());
    }
    auto size = reader.read_u64();
    if (!size) {
        return Result<DeltaOp>::failure(size.error());
    }
    auto data = reader.read_bytes();
    if (!data) {
        return Result<DeltaOp>::failure(data.error());
    }

    auto done = require_done(reader);
    if (!done) {
        return Result<DeltaOp>::failure(done.error());
    }

    return Result<DeltaOp>::success(DeltaOp{
        .type = static_cast<DeltaOpType>(raw_type.value()),
        .basis_offset = basis_offset.value(),
        .size = size.value(),
        .data = std::move(data).value(),
    });
}

}  // namespace lan
