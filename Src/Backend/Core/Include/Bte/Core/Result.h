#pragma once

#include <optional>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

namespace bte::core {

enum class ErrorCode {
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

    [[nodiscard]] const T& value() const& { return *value_; }

    [[nodiscard]] T&& value() && { return std::move(*value_); }

    [[nodiscard]] const Error& error() const noexcept { return error_; }

  private:
    std::optional<T> value_;
    Error error_;
};

} // namespace bte::core
