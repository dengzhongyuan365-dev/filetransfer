#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "lan/fs/file_hash.h"
#include "lan/protocol/frame.h"
#include "lan/transfer/block_signature.h"
#include "lan/transfer/chunk_codec.h"
#include "lan/transfer/delta.h"
#include "lan/transfer/file_metadata.h"
#include "lan/transfer/manifest.h"
#include "lan/transfer/sync_codec.h"
#include "lan/transfer/sync_executor.h"
#include "lan/transfer/sync_plan.h"

namespace {

class TempDir {
public:
    explicit TempDir(std::string name)
        : path_(std::filesystem::temp_directory_path() /
                (std::move(name) + "-" + std::to_string(::getpid()))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

TEST(FileMetadataTest, RoundTripsFileBeginMetadata) {
    lan::FileBeginMetadata metadata{
        .name = "demo.txt",
        .size = 42,
        .sha256 = "abcdef",
    };

    auto decoded = lan::decode_file_begin(lan::encode_file_begin(metadata));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().name, metadata.name);
    EXPECT_EQ(decoded.value().size, metadata.size);
    EXPECT_EQ(decoded.value().sha256, metadata.sha256);
}

TEST(ChunkCodecTest, RoundTripsOffsetAndPayload) {
    const std::vector<std::byte> payload = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    lan::Frame frame;
    frame.type = lan::MessageType::chunk;
    frame.body = lan::encode_chunk_body(17, payload.data(), payload.size());

    auto decoded = lan::decode_chunk_body(frame);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().offset, 17);
    ASSERT_EQ(decoded.value().size, payload.size());
    EXPECT_EQ(decoded.value().data[0], std::byte{'a'});
    EXPECT_EQ(decoded.value().data[2], std::byte{'c'});
}

TEST(ManifestTest, RecursivelyScansRegularFiles) {
    TempDir temp("manifest-test");
    write_text(temp.path() / "a.txt", "a");
    write_text(temp.path() / "sub" / "b.txt", "b");
    std::filesystem::create_directory_symlink(temp.path() / "sub", temp.path() / "link-to-sub");

    auto manifest = lan::build_manifest(temp.path(), 4096);
    ASSERT_TRUE(manifest);
    ASSERT_EQ(manifest.value().files.size(), 2);
    EXPECT_EQ(manifest.value().files[0].relative_path.generic_string(), "a.txt");
    EXPECT_EQ(manifest.value().files[1].relative_path.generic_string(), "sub/b.txt");
}

TEST(SyncCodecTest, RoundTripsManifestSyncPlanAndDeltaPlan) {
    lan::Manifest manifest;
    manifest.files.push_back(lan::ManifestEntry{
        .relative_path = "sub/a.txt",
        .size = 3,
        .sha256 = "hash",
        .mtime_ns = 9,
        .mode = 0644,
    });

    auto decoded_manifest = lan::decode_manifest(lan::encode_manifest(manifest));
    ASSERT_TRUE(decoded_manifest);
    ASSERT_EQ(decoded_manifest.value().files.size(), 1);
    EXPECT_EQ(decoded_manifest.value().files[0].relative_path.generic_string(), "sub/a.txt");

    lan::SyncPlan plan;
    plan.block_size = 8;
    lan::SyncPlanEntry entry;
    entry.action = lan::SyncAction::delta;
    entry.manifest_entry = manifest.files[0];
    entry.basis_signatures.push_back(lan::BlockSignature{
        .index = 0,
        .offset = 0,
        .size = 8,
        .weak_checksum = 123,
        .strong_checksum = "strong",
    });
    plan.entries.push_back(entry);

    auto decoded_plan = lan::decode_sync_plan(lan::encode_sync_plan(plan));
    ASSERT_TRUE(decoded_plan);
    EXPECT_EQ(decoded_plan.value().block_size, 8);
    ASSERT_EQ(decoded_plan.value().entries.size(), 1);
    ASSERT_EQ(decoded_plan.value().entries[0].basis_signatures.size(), 1);

    lan::DeltaPlan delta;
    delta.source_size = 2;
    delta.source_sha256 = "sha";
    delta.op_count = 1;
    delta.ops.push_back(lan::DeltaOp{
        .type = lan::DeltaOpType::literal_data,
        .size = 2,
        .data = {std::byte{'o'}, std::byte{'k'}},
    });

    auto decoded_delta = lan::decode_delta_plan(lan::encode_delta_plan(delta));
    ASSERT_TRUE(decoded_delta);
    EXPECT_EQ(decoded_delta.value().source_sha256, "sha");
    ASSERT_EQ(decoded_delta.value().ops.size(), 1);

    auto decoded_header = lan::decode_delta_header(lan::encode_delta_header(delta));
    ASSERT_TRUE(decoded_header);
    EXPECT_EQ(decoded_header.value().op_count, 1);

    auto decoded_op = lan::decode_delta_op(lan::encode_delta_op(delta.ops[0]));
    ASSERT_TRUE(decoded_op);
    EXPECT_EQ(decoded_op.value().data.size(), 2);
}

TEST(DeltaTest, RebuildsSourceFromBasisAndDelta) {
    TempDir temp("delta-test");
    const auto basis = temp.path() / "basis.txt";
    const auto source = temp.path() / "source.txt";
    const auto rebuilt = temp.path() / "rebuilt.txt";

    write_text(basis, "aaaa-bbbb-cccc-dddd-eeee\n");
    write_text(source, "xxxx-bbbb-cccc-yyyy-eeee\n");

    auto signatures = lan::build_block_signatures(basis, 5);
    ASSERT_TRUE(signatures);

    auto delta = lan::build_delta(source, signatures.value(), 5);
    ASSERT_TRUE(delta);
    EXPECT_GT(delta.value().ops.size(), 0);

    auto applied = lan::apply_delta(basis, rebuilt, delta.value().ops);
    ASSERT_TRUE(applied);
    EXPECT_EQ(read_text(rebuilt), read_text(source));
}

TEST(SyncPlanAndExecutorTest, HandlesSkipFullAndDelta) {
    TempDir source("sync-source-test");
    TempDir receive("sync-receive-test");

    write_text(source.path() / "same.txt", "same\n");
    write_text(receive.path() / "same.txt", "same\n");
    write_text(source.path() / "sub" / "new.txt", "new nested\n");
    write_text(source.path() / "changed.txt", "xxxx-bbbb-cccc-yyyy-eeee\n");
    write_text(receive.path() / "changed.txt", "aaaa-bbbb-cccc-dddd-eeee\n");

    auto manifest = lan::build_manifest(source.path(), 4096);
    ASSERT_TRUE(manifest);

    auto plan = lan::build_sync_plan(manifest.value(), receive.path(), 5);
    ASSERT_TRUE(plan);

    auto report = lan::execute_local_sync(manifest.value(), source.path(), plan.value());
    ASSERT_TRUE(report);
    EXPECT_EQ(report.value().skipped_files, 1);
    EXPECT_EQ(report.value().full_files, 1);
    EXPECT_EQ(report.value().delta_files, 1);

    for (const auto& entry : manifest.value().files) {
        auto src_hash = lan::hash_file(source.path() / entry.relative_path);
        auto dst_hash = lan::hash_file(receive.path() / entry.relative_path);
        ASSERT_TRUE(src_hash);
        ASSERT_TRUE(dst_hash);
        EXPECT_EQ(src_hash.value().hex_digest, dst_hash.value().hex_digest);
    }
}

}  // namespace
