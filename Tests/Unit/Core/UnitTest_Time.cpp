#include "Bte/Core/Time.h"

#include <gtest/gtest.h>
#include <string_view>

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

TEST(TimeTest, parsesCompleteUtcMillisecondsAndRejectsUnsupportedInput) {
  const auto dateOnly = bte::core::time::parseIso8601("2024-01-02");
  ASSERT_TRUE(dateOnly.ok()) << dateOnly.error().message;
  EXPECT_EQ(bte::core::time::toUnixMillis(dateOnly.value()), 1'704'153'600'000);

  const auto parsed = bte::core::time::parseIso8601("2024-01-02T00:00:00.125Z");
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(bte::core::time::toUnixMillis(parsed.value()), 1'704'153'600'125);
  EXPECT_EQ(bte::core::time::toIso8601(parsed.value()),
            "2024-01-02T00:00:00.125Z");
  const auto beforeEpoch =
      bte::core::time::parseIso8601("1969-12-31T23:59:59.999Z");
  ASSERT_TRUE(beforeEpoch.ok());
  EXPECT_EQ(bte::core::time::toUnixMillis(beforeEpoch.value()), -1);
  EXPECT_EQ(bte::core::time::toIso8601(beforeEpoch.value()),
            "1969-12-31T23:59:59.999Z");
  const auto oneFractionalDigit =
      bte::core::time::parseIso8601("2024-01-02T00:00:00.1Z");
  const auto twoFractionalDigits =
      bte::core::time::parseIso8601("2024-01-02T00:00:00.12+00:00");
  const auto threeFractionalDigits =
      bte::core::time::parseIso8601("2024-01-02T00:00:00.125");
  ASSERT_TRUE(oneFractionalDigit.ok());
  ASSERT_TRUE(twoFractionalDigits.ok());
  ASSERT_TRUE(threeFractionalDigits.ok());
  EXPECT_EQ(bte::core::time::toUnixMillis(oneFractionalDigit.value()),
            1'704'153'600'100);
  EXPECT_EQ(bte::core::time::toUnixMillis(twoFractionalDigits.value()),
            1'704'153'600'120);
  EXPECT_EQ(bte::core::time::toUnixMillis(threeFractionalDigits.value()),
            1'704'153'600'125);
  for (const std::string_view invalid :
       {"2024-01-02T00:00:00+08:00", "2024-01-02T12:3",
        "2024-01-02T00:00:00Zextra", "2024-01-02T00:00:00.1234Z",
        "2024-01-02T00:00:00.Z", "2024-01-02T23:59:60Z",
        "-001-01-02T00:00:00Z"}) {
    const auto result = bte::core::time::parseIso8601(invalid);
    EXPECT_FALSE(result.ok()) << invalid;
    EXPECT_EQ(result.error().code, bte::core::ErrorCode::invalidArgument);
  }
}
