#include "Bte/Core/Time.h"

#include <gtest/gtest.h>

TEST(TimeTest, parseIso8601_acceptsDuckDbUtcTimestamp) {
  auto timestamp = bte::core::time::parseIso8601("2024-01-02 03:04:05+00:00");

  ASSERT_TRUE(timestamp.ok()) << timestamp.error().message;
  EXPECT_EQ(bte::core::time::toIso8601(timestamp.value()),
            "2024-01-02T03:04:05Z");
}

TEST(TimeTest, parseIso8601_rejectsInvalidTimestamp) {
  auto timestamp = bte::core::time::parseIso8601("not-a-time");

  ASSERT_FALSE(timestamp.ok());
  EXPECT_EQ(timestamp.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(TimeTest, parseIso8601_rejectsMalformedDateAndTimeFields) {
  const auto malformedDate =
      bte::core::time::parseIso8601("202x-01-02 03:04:05");
  const auto malformedTime =
      bte::core::time::parseIso8601("2024-01-02 03-04-05");

  ASSERT_FALSE(malformedDate.ok());
  ASSERT_FALSE(malformedTime.ok());
  EXPECT_EQ(malformedDate.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(malformedTime.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(TimeTest, parseIso8601_rejectsCalendarValuesOutsideSupportedRange) {
  const auto invalidDate = bte::core::time::parseIso8601("2024-02-30 03:04:05");
  const auto invalidHour = bte::core::time::parseIso8601("2024-01-02 24:04:05");

  ASSERT_FALSE(invalidDate.ok());
  ASSERT_FALSE(invalidHour.ok());
  EXPECT_EQ(invalidDate.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(invalidHour.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(TimeTest, unixMillisRoundTrip_preservesTimestamp) {
  const auto timestamp = bte::core::time::fromUnixMillis(1'704'159'845'000);

  EXPECT_EQ(bte::core::time::toUnixMillis(timestamp), 1'704'159'845'000);
}
