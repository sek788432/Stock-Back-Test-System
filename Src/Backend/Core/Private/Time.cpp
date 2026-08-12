#include "Bte/Core/Time.h"

#include <charconv>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace bte::core::time {
namespace {

[[nodiscard]] bool parseFixedInt(std::string_view text, int& value) {
    const auto* begin = text.data();
    const auto* end = begin + text.size();
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

} // namespace

Result<Timestamp> parseIso8601(std::string_view text) {
    if (text.size() >= 10 && text[4] == '-' && text[7] == '-') {
        int year = 0;
        int month = 0;
        int day = 0;
        if (!parseFixedInt(text.substr(0, 4), year) || !parseFixedInt(text.substr(5, 2), month) ||
            !parseFixedInt(text.substr(8, 2), day)) {
            return makeError(ErrorCode::invalidArgument, "timestamp contains non-numeric date fields");
        }

        int hour = 0;
        int minute = 0;
        int second = 0;
        if (text.size() >= 19) {
            if ((text[10] != ' ' && text[10] != 'T') || text[13] != ':' || text[16] != ':' ||
                !parseFixedInt(text.substr(11, 2), hour) || !parseFixedInt(text.substr(14, 2), minute) ||
                !parseFixedInt(text.substr(17, 2), second)) {
                return makeError(ErrorCode::invalidArgument, "timestamp contains invalid time fields");
            }
        }

        using namespace std::chrono;
        const auto date = sys_days{std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} /
                                   std::chrono::day{static_cast<unsigned>(day)}};
        if (year_month_day{date}.year() != std::chrono::year{year} ||
            year_month_day{date}.month() != std::chrono::month{static_cast<unsigned>(month)} ||
            year_month_day{date}.day() != std::chrono::day{static_cast<unsigned>(day)} || hour < 0 || hour > 23 ||
            minute < 0 || minute > 59 || second < 0 || second > 60) {
            return makeError(ErrorCode::invalidArgument, "timestamp is outside the supported calendar range");
        }

        return Timestamp{
            duration_cast<milliseconds>(date.time_since_epoch() + hours{hour} + minutes{minute} + seconds{second})};
    }

    return makeError(ErrorCode::invalidArgument, "timestamp must use ISO-8601 format");
}

std::string toIso8601(const Timestamp timestamp) {
    const auto millis = toUnixMillis(timestamp);
    const auto seconds = static_cast<std::time_t>(millis / 1000);
    const auto utc = toUtcTm(seconds);

    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

Timestamp fromUnixMillis(const std::int64_t milliseconds) { return Timestamp{std::chrono::milliseconds{milliseconds}}; }

std::int64_t toUnixMillis(const Timestamp timestamp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count();
}

} // namespace bte::core::time
