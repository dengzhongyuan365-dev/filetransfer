#include <gtest/gtest.h>

#include "lan/app/receiver_config.h"
#include "lan/common/parse.h"
#include "lan/common/size.h"

namespace {

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

}  // namespace
