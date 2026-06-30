#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unistd.h>

#include "lan/app/receiver_config.h"
#include "lan/app/sender_config.h"
#include "lan/common/error.h"
#include "lan/common/parse.h"
#include "lan/common/size.h"

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

TEST(ParseSizeTest, ParsesBinaryUnits) {
    auto bytes = lan::parse_size("4MiB");
    ASSERT_TRUE(bytes);
    EXPECT_EQ(bytes.value(), 4ull * 1024ull * 1024ull);

    auto kib = lan::parse_size("7KiB");
    ASSERT_TRUE(kib);
    EXPECT_EQ(kib.value(), 7ull * 1024ull);
}

TEST(ParseSizeTest, RejectsInvalidText) {
    auto result = lan::parse_size("12wat");
    EXPECT_FALSE(result);
}

TEST(FormatSizeTest, FormatsBytesAndBinaryUnits) {
    EXPECT_EQ(lan::format_size(27), "27 B");
    EXPECT_EQ(lan::format_size(1536), "1.50 KiB");
}

TEST(FormatRateTest, FormatsBytesPerSecond) {
    EXPECT_EQ(lan::format_rate(2048, 2.0), "1.00 KiB/s");
    EXPECT_EQ(lan::format_rate(2048, 0.0), "n/a");
}

TEST(ErrorTest, ExposesStableNamesAndCategories) {
    EXPECT_EQ(lan::error_code_name(lan::ErrorCode::network_error), "network_error");
    EXPECT_EQ(lan::error_category(lan::ErrorCode::network_error), lan::ErrorCategory::network);
    EXPECT_EQ(lan::error_category_name(lan::ErrorCategory::network), "network");
    EXPECT_EQ(lan::error_category(lan::ErrorCode::checksum_mismatch),
              lan::ErrorCategory::integrity);
}

TEST(ErrorTest, ClassifiesRetryAndUserAction) {
    EXPECT_TRUE(lan::is_retryable(lan::ErrorCode::network_error));
    EXPECT_TRUE(lan::is_retryable(lan::ErrorCode::io_error));
    EXPECT_FALSE(lan::is_retryable(lan::ErrorCode::invalid_argument));
    EXPECT_FALSE(lan::is_retryable(lan::ErrorCode::checksum_mismatch));

    EXPECT_TRUE(lan::needs_user_action(lan::ErrorCode::invalid_argument));
    EXPECT_TRUE(lan::needs_user_action(lan::ErrorCode::checksum_mismatch));
    EXPECT_FALSE(lan::needs_user_action(lan::ErrorCode::network_error));
    EXPECT_FALSE(lan::needs_user_action(lan::ErrorCode::cancelled));
}

TEST(ErrorTest, FormatsErrorsWithCode) {
    lan::Error error{lan::ErrorCode::protocol_error, "expected hello frame"};

    EXPECT_EQ(lan::format_error(error), "[protocol_error] expected hello frame");
}

TEST(ReceiverConfigTest, ParsesOnceMode) {
    const char* args[] = {"receiver", "--port", "9000", "--dir", "/tmp", "--once"};
    auto config = lan::parse_receiver_args(6, const_cast<char**>(args));
    ASSERT_TRUE(config);
    EXPECT_TRUE(config.value().once);
}

TEST(ReceiverConfigTest, DefaultsToPersistentMode) {
    const char* args[] = {"receiver", "--port", "9000", "--dir", "/tmp"};
    auto config = lan::parse_receiver_args(5, const_cast<char**>(args));
    ASSERT_TRUE(config);
    EXPECT_FALSE(config.value().once);
}

TEST(SenderConfigTest, ParsesAllOptions) {
    const char* args[] = {
        "sender",
        "--host",
        "127.0.0.1",
        "--port",
        "39123",
        "--path",
        "demo.txt",
        "--chunk-size",
        "8KiB",
        "--no-resume",
    };

    auto config = lan::parse_sender_args(10, const_cast<char**>(args));

    ASSERT_TRUE(config) << config.error().message;
    EXPECT_EQ(config.value().target.host, "127.0.0.1");
    EXPECT_EQ(config.value().target.port, 39123);
    EXPECT_EQ(config.value().source_path, "demo.txt");
    EXPECT_EQ(config.value().chunk_size, 8ull * 1024ull);
    EXPECT_FALSE(config.value().resume);
}

TEST(SenderConfigTest, RejectsMissingHost) {
    const char* args[] = {"sender", "--port", "39123", "--path", "demo.txt"};

    auto config = lan::parse_sender_args(5, const_cast<char**>(args));

    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, lan::ErrorCode::invalid_argument);
}

TEST(SenderConfigTest, ValidatesExistingSourcePath) {
    TempDir temp("sender-config-test");
    const auto source = temp.path() / "source.txt";
    write_text(source, "hello");

    lan::SenderConfig config;
    config.target.host = "127.0.0.1";
    config.target.port = 39123;
    config.source_path = source;

    auto validated = lan::validate_sender_config(std::move(config));

    ASSERT_TRUE(validated) << validated.error().message;
    EXPECT_EQ(validated.value().source_path, source);
}

TEST(SenderConfigTest, RejectsMissingSourcePath) {
    lan::SenderConfig config;
    config.target.host = "127.0.0.1";
    config.target.port = 39123;
    config.source_path = "/tmp/lan-file-transfer-missing-source";

    auto validated = lan::validate_sender_config(std::move(config));

    ASSERT_FALSE(validated);
    EXPECT_EQ(validated.error().code, lan::ErrorCode::not_found);
}

TEST(SenderConfigTest, RejectsInvalidChunkSizeDuringValidation) {
    TempDir temp("sender-chunk-size-test");
    const auto source = temp.path() / "source.txt";
    write_text(source, "hello");

    lan::SenderConfig zero_config;
    zero_config.target.host = "127.0.0.1";
    zero_config.target.port = 39123;
    zero_config.source_path = source;
    zero_config.chunk_size = 0;

    auto zero = lan::validate_sender_config(std::move(zero_config));
    ASSERT_FALSE(zero);
    EXPECT_EQ(zero.error().code, lan::ErrorCode::invalid_argument);

    lan::SenderConfig oversized_config;
    oversized_config.target.host = "127.0.0.1";
    oversized_config.target.port = 39123;
    oversized_config.source_path = source;
    oversized_config.chunk_size =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;

    auto oversized = lan::validate_sender_config(std::move(oversized_config));
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, lan::ErrorCode::invalid_argument);
}

TEST(ReceiverConfigTest, ParsesBindOverwriteAndBlockSize) {
    const char* args[] = {
        "receiver",
        "--bind",
        "127.0.0.1",
        "--port",
        "39123",
        "--dir",
        "/tmp",
        "--allow-overwrite",
        "--block-size",
        "16KiB",
    };

    auto config = lan::parse_receiver_args(10, const_cast<char**>(args));

    ASSERT_TRUE(config) << config.error().message;
    EXPECT_EQ(config.value().bind_address, "127.0.0.1");
    EXPECT_EQ(config.value().port, 39123);
    EXPECT_EQ(config.value().receive_dir, "/tmp");
    EXPECT_TRUE(config.value().allow_overwrite);
    EXPECT_EQ(config.value().block_size, 16ull * 1024ull);
}

TEST(ReceiverConfigTest, ValidateCreatesReceiveDirectory) {
    TempDir temp("receiver-config-test");
    const auto receive_dir = temp.path() / "nested" / "receive";

    lan::ReceiverConfig config;
    config.port = 39123;
    config.receive_dir = receive_dir;

    auto validated = lan::validate_receiver_config(std::move(config));

    ASSERT_TRUE(validated) << validated.error().message;
    EXPECT_EQ(validated.value().receive_dir, receive_dir);
    EXPECT_TRUE(std::filesystem::is_directory(receive_dir));
}

TEST(ReceiverConfigTest, RejectsOversizedBlockSize) {
    TempDir temp("receiver-block-size-test");

    lan::ReceiverConfig config;
    config.port = 39123;
    config.receive_dir = temp.path();
    config.block_size = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;

    auto validated = lan::validate_receiver_config(std::move(config));

    ASSERT_FALSE(validated);
    EXPECT_EQ(validated.error().code, lan::ErrorCode::invalid_argument);
}

TEST(ReceiverConfigTest, RejectsZeroBlockSizeDuringValidation) {
    TempDir temp("receiver-zero-block-size-test");

    lan::ReceiverConfig config;
    config.port = 39123;
    config.receive_dir = temp.path();
    config.block_size = 0;

    auto validated = lan::validate_receiver_config(std::move(config));

    ASSERT_FALSE(validated);
    EXPECT_EQ(validated.error().code, lan::ErrorCode::invalid_argument);
}

}  // namespace
