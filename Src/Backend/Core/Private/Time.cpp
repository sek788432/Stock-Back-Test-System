#include "Bte/Core/Time.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <functional> // IWYU pragma: keep
#include <iomanip>
#include <ratio> // IWYU pragma: keep
#include <sstream>
#include <system_error>

namespace bte::core::time {
namespace {

[[nodiscard]] bool parseFixedInt(std::string_view text, int &value) {
  if (text.empty() || !std::ranges::all_of(text, [](char digit) {
        return digit >= '0' && digit <= '9';
      })) {
    return false;
  }
  const auto *begin = text.data();
  const auto *end = begin + text.size();
  const auto result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] std::tm toUtcTm(const std::time_t seconds) {
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  return utc;
}

[[nodiscard]] Result<std::chrono::sys_days>
parseCalendarDate(const std::string_view text) {
  int year = 0;
  int month = 0;
  int day = 0;
  if (!parseFixedInt(text.substr(0, 4), year) ||
      !parseFixedInt(text.substr(5, 2), month) ||
      !parseFixedInt(text.substr(8, 2), day)) {
    return makeError(ErrorCode::invalidArgument,
                     "timestamp contains non-numeric date fields");
  }

  const auto date =
      std::chrono::sys_days{std::chrono::year{year} /
                            std::chrono::month{static_cast<unsigned>(month)} /
                            std::chrono::day{static_cast<unsigned>(day)}};
  const auto calendarDate = std::chrono::year_month_day{date};
  if (calendarDate.year() != std::chrono::year{year} ||
      calendarDate.month() !=
          std::chrono::month{static_cast<unsigned>(month)} ||
      calendarDate.day() != std::chrono::day{static_cast<unsigned>(day)}) {
    return makeError(ErrorCode::invalidArgument,
                     "timestamp is outside the supported calendar range");
  }
  return date;
}

[[nodiscard]] Result<std::chrono::milliseconds>
parseUtcFraction(const std::string_view suffix) {
  auto remainder = suffix;
  int millisecond = 0;
  if (remainder.starts_with('.')) {
    remainder.remove_prefix(1);
    const auto nonDigit = remainder.find_first_not_of("0123456789");
    const auto digits =
        nonDigit == std::string_view::npos ? remainder.size() : nonDigit;
    if (digits == 0 || digits > 3 ||
        !parseFixedInt(remainder.substr(0, digits), millisecond)) {
      return makeError(ErrorCode::invalidArgument,
                       "timestamp has unsupported fractional precision");
    }
    for (auto index = digits; index < 3; ++index) {
      millisecond *= 10;
    }
    remainder.remove_prefix(digits);
  }
  if (!remainder.empty() && remainder != "Z" && remainder != "+00:00") {
    return makeError(ErrorCode::invalidArgument,
                     "timestamp has an unsupported UTC offset or suffix");
  }
  return std::chrono::milliseconds{millisecond};
}

[[nodiscard]] Result<std::chrono::milliseconds>
parseTimeOfDay(const std::string_view text) {
  if (text.size() == 10) {
    return std::chrono::milliseconds{0};
  }
  if (text.size() < 19) {
    return makeError(ErrorCode::invalidArgument,
                     "timestamp contains incomplete time fields");
  }

  int hour = 0;
  int minute = 0;
  int second = 0;
  if ((text[10] != ' ' && text[10] != 'T') || text[13] != ':' ||
      text[16] != ':' || !parseFixedInt(text.substr(11, 2), hour) ||
      !parseFixedInt(text.substr(14, 2), minute) ||
      !parseFixedInt(text.substr(17, 2), second)) {
    return makeError(ErrorCode::invalidArgument,
                     "timestamp contains invalid time fields");
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
      second > 59) {
    return makeError(ErrorCode::invalidArgument,
                     "timestamp is outside the supported calendar range");
  }
  auto fraction = parseUtcFraction(text.substr(19));
  if (!fraction.ok()) {
    return fraction.error();
  }
  return std::chrono::hours{hour} + std::chrono::minutes{minute} +
         std::chrono::seconds{second} + fraction.value();
}

} // namespace

Result<Timestamp> parseIso8601(std::string_view text) {
  if (text.size() < 10 || text[4] != '-' || text[7] != '-') {
    return makeError(ErrorCode::invalidArgument,
                     "timestamp must use ISO-8601 format");
  }

  auto date = parseCalendarDate(text);
  if (!date.ok()) {
    return date.error();
  }
  auto time = parseTimeOfDay(text);
  if (!time.ok()) {
    return time.error();
  }
  return Timestamp{std::chrono::duration_cast<std::chrono::milliseconds>(
                       date.value().time_since_epoch()) +
                   time.value()};
}

std::string toIso8601(const Timestamp timestamp) {
  const auto wholeSeconds = std::chrono::floor<std::chrono::seconds>(timestamp);
  const auto fraction = std::chrono::duration_cast<std::chrono::milliseconds>(
                            timestamp - wholeSeconds)
                            .count();
  const auto seconds =
      static_cast<std::time_t>(wholeSeconds.time_since_epoch().count());
  const auto utc = toUtcTm(seconds);

  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S");
  if (fraction != 0) {
    out << '.' << std::setfill('0') << std::setw(3) << fraction;
  }
  out << 'Z';
  return out.str();
}

Timestamp fromUnixMillis(const std::int64_t milliseconds) {
  return Timestamp{std::chrono::milliseconds{milliseconds}};
}

std::int64_t toUnixMillis(const Timestamp timestamp) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             timestamp.time_since_epoch())
      .count();
}

} // namespace bte::core::time
