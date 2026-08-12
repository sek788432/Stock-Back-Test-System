#pragma once

#include <cstdint>
#include <optional>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

namespace bte::core {

enum class ErrorCode : std::uint8_t {
    ok = 0,
    invalidArgument,
    notFound,
    permissionDenied,
    cancelled,
    timeout,
    internal,
    dataUnavailable,
    schemaMismatch,
    strategyCompileFailed,
    strategyRuntimeError,
    insufficientCash,
    insufficientShares,
    pluginIncompatibleAbi,
};

struct Error {
    ErrorCode code = ErrorCode::ok;
    std::string message;
    std::source_location where = std::source_location::current();
    std::vector<Error> causes;

    explicit operator bool() const noexcept { return code != ErrorCode::ok; }
};

[[nodiscard]] Error makeError(ErrorCode code, std::string message,
                              std::source_location where = std::source_location::current());

template <typename T> class Result {
  public:
    Result(T value) : value_(std::move(value)) {}

    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return !error_; }

    [[nodiscard]] const T& value() const& {
        // std::optional::value performs the check and throws; the contract is
        // covered by ResultTest.valueAccessOnErrorThrowsForConstAndMovedResults.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        return value_.value();
    }

    [[nodiscard]] T&& value() && {
        // std::optional::value performs the check and throws; the contract is
        // covered by ResultTest.valueAccessOnErrorThrowsForConstAndMovedResults.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        return std::move(value_).value();
    }

    [[nodiscard]] const Error& error() const noexcept { return error_; }

  private:
    std::optional<T> value_;
    Error error_;
};

} // namespace bte::core
