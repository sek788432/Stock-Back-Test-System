#include "Bte/Core/Time.h"

#include <gtest/gtest.h>

TEST(TimeTest, parseIso8601_acceptsDuckDbUtcTimestamp) {
    auto timestamp = bte::core::time::parseIso8601("2024-01-02 03:04:05+00:00");

    ASSERT_TRUE(timestamp.ok()) << timestamp.error().message;
    EXPECT_EQ(bte::core::time::toIso8601(timestamp.value()), "2024-01-02T03:04:05Z");
}

TEST(TimeTest, parseIso8601_rejectsInvalidTimestamp) {
    auto timestamp = bte::core::time::parseIso8601("not-a-time");

    ASSERT_FALSE(timestamp.ok());
    EXPECT_EQ(timestamp.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(TimeTest, unixMillisRoundTrip_preservesTimestamp) {
    const auto timestamp = bte::core::time::fromUnixMillis(1'704'159'845'000);

    EXPECT_EQ(bte::core::time::toUnixMillis(timestamp), 1'704'159'845'000);
}
