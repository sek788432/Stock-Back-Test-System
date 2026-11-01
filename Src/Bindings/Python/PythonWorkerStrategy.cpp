#include "PythonWorkerStrategy.h"

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Digest.h"
#include "Bte/Core/Result.h"
#include "Bte/Core/Time.h"
#include "Bte/Indicators/StreamingIndicator.h"
#include "Bte/Strategy/Strategy.h"
#include "StrategyProtocol.h"

// IWYU pragma: no_include <math>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional> // IWYU pragma: keep
#include <limits>
#include <nlohmann/detail/input/json_sax.hpp>      // IWYU pragma: keep
#include <nlohmann/detail/iterators/iter_impl.hpp> // IWYU pragma: keep
#include <nlohmann/detail/json_ref.hpp>
#include <nlohmann/detail/output/serializer.hpp>
#include <optional>
#include <random>
#include <ratio> // IWYU pragma: keep
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits> // IWYU pragma: keep
#include <utility>

namespace bte::bindings::python {
namespace {

using namespace std::chrono_literals;

constexpr auto bootstrapDeadline = 10s;
constexpr auto handshakeDeadline = 5s;
constexpr auto moduleDeadline = 10s;
constexpr auto factoryDeadline = 10s;
constexpr auto hookDeadline = 30s;
constexpr auto shutdownDeadline = 2s;
constexpr std::size_t maximumStructuredLogEntries = 10'000;
constexpr std::size_t maximumStructuredLogBytes = std::size_t{1024} * 1024;
constexpr std::size_t maximumLogMessageBytes = std::size_t{8} * 1024;
constexpr double microdollarsPerDollar = 1'000'000.0;
constexpr double nanodollarsPerDollar = 1'000'000'000.0;
constexpr std::string_view base64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] core::Error makeStrategyError(const core::ErrorCode code,
                                            std::string message) {
  return core::makeError(code, std::move(message));
} // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.

[[nodiscard]] std::string_view
strategyDiagnosticCode(const core::ErrorCode code) {
  switch (code) {
  case core::ErrorCode::cancelled:
    return "Canceled";
  case core::ErrorCode::strategyHookTimeout:
    return "StrategyHookTimeout";
  case core::ErrorCode::strategyProtocolViolation:
    return "StrategyProtocolViolation";
  case core::ErrorCode::strategyCommandRejected:
    return "StrategyCommandRejected";
  case core::ErrorCode::strategyRuntimeError:
    return "strategyRuntimeError";
  case core::ErrorCode::invalidHistoryRequest:
    return "InvalidHistoryRequest";
  case core::ErrorCode::historyUnavailable:
    return "HistoryUnavailable";
  case core::ErrorCode::historyBudgetExceeded:
    return "HistoryBudgetExceeded";
  case core::ErrorCode::futureDataRequest:
    return "FutureDataRequest";
  case core::ErrorCode::invalidStrategyEncoding:
    return "InvalidStrategyEncoding";
  case core::ErrorCode::strategySourceTooLarge:
    return "StrategySourceTooLarge";
  case core::ErrorCode::strategySourceContainsNul:
    return "StrategySourceContainsNul";
  case core::ErrorCode::strategySyntaxError:
    return "StrategySyntaxError";
  case core::ErrorCode::strategyImportNotAllowed:
    return "StrategyImportNotAllowed";
  case core::ErrorCode::unsupportedStrategyApiVersion:
    return "UnsupportedStrategyApiVersion";
  case core::ErrorCode::invalidStrategyParameters:
    return "InvalidStrategyParameters";
  case core::ErrorCode::strategyConsentRequired:
    return "StrategyConsentRequired";
  default:
    return "WorkerCallbackFailure";
  }
}

void storeTerminalDiagnostic(
    const std::string_view errorCode, const std::string_view message,
    const std::string_view hook, const std::optional<std::uint32_t> sourceLine,
    const std::optional<std::uint32_t> sourceColumn,
    const std::string_view traceback, const std::uint64_t omittedBytes,
    const std::uint64_t omittedEntries, PythonStrategyDiagnostics &diagnostics,
    strategy::StrategyDiagnosticBatch &pending) {
  pending.records.push_back(strategy::StrategyDiagnostic{
      .kind = strategy::StrategyDiagnosticKind::terminal,
      .hook = std::string{hook},
      .level = "error",
      .errorCode = std::string{errorCode},
      .message = std::string{message},
      .sourceLine = sourceLine,
      .sourceColumn = sourceColumn,
      .traceback = std::string{traceback},
      .omittedBytes = omittedBytes,
      .omittedEntries = omittedEntries,
  });
  diagnostics.structuredErrors.push_back(nlohmann::json{
      {"errorCode", errorCode},
      {"hook", hook},
      {"message", message},
      {"sourceColumn", sourceColumn.has_value()
                           ? nlohmann::json(sourceColumn.value())
                           : nlohmann::json(nullptr)},
      {"sourceLine", sourceLine.has_value() ? nlohmann::json(sourceLine.value())
                                            : nlohmann::json(nullptr)},
      {"traceback", traceback},
      {"omittedBytes", omittedBytes},
      {"omittedEntries", omittedEntries},
  });
} // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.

[[nodiscard]] bool hasExactKeys(const nlohmann::json &value,
                                const std::set<std::string> &keys) {
  if (!value.is_object() || value.size() != keys.size()) {
    return false;
  }
  return std::ranges::all_of(
      keys, [&value](const std::string &key) { return value.contains(key); });
}

[[nodiscard]] bool isStrictUtf8(const std::string_view value) {
  try {
    static_cast<void>(
        nlohmann::json(std::string{value})
            .dump(-1, ' ', false, nlohmann::json::error_handler_t::strict));
    return true;
  } catch (const nlohmann::json::exception &) {
    return false;
  }
}

[[nodiscard]] core::Result<std::string> normalizeSource(std::string source) {
  if (source.starts_with("\xef\xbb\xbf")) {
    source.erase(0, 3);
  }
  std::string normalized;
  normalized.reserve(source.size());
  for (std::size_t index = 0; index < source.size(); ++index) {
    const auto character = source[index];
    if (character == '\0') {
      return makeStrategyError(core::ErrorCode::strategySourceContainsNul,
                               "Strategy source contains a NUL byte.");
    }
    if (character == '\r') {
      normalized.push_back('\n');
      if (index + 1 < source.size() && source[index + 1] == '\n') {
        ++index;
      }
    } else {
      normalized.push_back(character);
    }
  }
  if (normalized.empty()) {
    return makeStrategyError(core::ErrorCode::invalidArgument,
                             "Strategy source must contain at least one byte.");
  }
  if (normalized.size() > maximumWorkerSourceBytes) {
    return makeStrategyError(core::ErrorCode::strategySourceTooLarge,
                             "Strategy source must not exceed 1 MiB.");
  }
  if (!isStrictUtf8(normalized)) {
    return makeStrategyError(core::ErrorCode::invalidStrategyEncoding,
                             "Strategy source is not valid UTF-8.");
  }
  return normalized;
}

[[nodiscard]] bool readWholeInteger(const nlohmann::json &value,
                                    const std::int64_t minimum,
                                    const std::int64_t maximum,
                                    std::int64_t &result) {
  if (value.is_boolean()) {
    return false;
  }
  if (value.is_number_unsigned()) {
    const auto unsignedValue = value.get<std::uint64_t>();
    if (unsignedValue > static_cast<std::uint64_t>(maximum)) {
      return false;
    }
    result = static_cast<std::int64_t>(unsignedValue);
  } else if (value.is_number_integer()) {
    result = value.get<std::int64_t>();
  } else {
    return false;
  }
  return result >= minimum && result <= maximum;
}

[[nodiscard]] bool isValidSmaName(const std::string_view name) {
  if (name.empty() || name.size() > 64) {
    return false;
  }
  const auto isAsciiLetter = [](const char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
  };
  return isAsciiLetter(name.front()) &&
         std::ranges::all_of(name.substr(1), [&](const char character) {
           return isAsciiLetter(character) ||
                  (character >= '0' && character <= '9') || character == '_';
         });
}

[[nodiscard]] std::string encodeBase64(const std::string_view bytes) {
  std::string encoded;
  encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);
  for (std::size_t offset = 0; offset < bytes.size(); offset += 3U) {
    const auto first = static_cast<unsigned char>(bytes[offset]);
    const auto hasSecond = offset + 1U < bytes.size();
    const auto hasThird = offset + 2U < bytes.size();
    const auto second =
        hasSecond ? static_cast<unsigned char>(bytes[offset + 1U]) : 0U;
    const auto third =
        hasThird ? static_cast<unsigned char>(bytes[offset + 2U]) : 0U;
    const auto combined = (static_cast<std::uint32_t>(first) << 16U) |
                          (static_cast<std::uint32_t>(second) << 8U) |
                          static_cast<std::uint32_t>(third);
    encoded.push_back(base64Alphabet[(combined >> 18U) & 0x3fU]);
    encoded.push_back(base64Alphabet[(combined >> 12U) & 0x3fU]);
    encoded.push_back(hasSecond ? base64Alphabet[(combined >> 6U) & 0x3fU]
                                : '=');
    encoded.push_back(hasThird ? base64Alphabet[combined & 0x3fU] : '=');
  }
  return encoded;
}

[[nodiscard]] std::string
encodeHistoryBars(const std::span<const core::Bar> bars) {
  std::string bytes;
  bytes.reserve(bars.size() * 48U);
  const auto appendBigEndian = [&bytes](const std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      bytes.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
  };
  for (const auto &bar : bars) {
    appendBigEndian(
        static_cast<std::uint64_t>(core::time::toUnixMillis(bar.ts)));
    appendBigEndian(std::bit_cast<std::uint64_t>(bar.open));
    appendBigEndian(std::bit_cast<std::uint64_t>(bar.high));
    appendBigEndian(std::bit_cast<std::uint64_t>(bar.low));
    appendBigEndian(std::bit_cast<std::uint64_t>(bar.close));
    appendBigEndian(std::bit_cast<std::uint64_t>(bar.volume));
  }
  return bytes;
}

[[nodiscard]] core::Result<std::string>
validateParameters(const nlohmann::json &parameters) {
  static const std::set<std::string> expectedKeys{"fast_period", "quantity",
                                                  "slow_period"};
  if (!hasExactKeys(parameters, expectedKeys)) {
    return makeStrategyError(core::ErrorCode::invalidStrategyParameters,
                             "Python Strategy parameters must contain exactly "
                             "fast_period, slow_period, and quantity.");
  }
  std::int64_t fastPeriod = 0;
  std::int64_t slowPeriod = 0;
  std::int64_t quantity = 0;
  if (!readWholeInteger(parameters.at("fast_period"), 1, 10'000, fastPeriod) ||
      !readWholeInteger(parameters.at("slow_period"), 1, 10'000, slowPeriod) ||
      !readWholeInteger(parameters.at("quantity"), 1,
                        strategy::maximumStrategyQuantityShares, quantity) ||
      fastPeriod >= slowPeriod) {
    return makeStrategyError(core::ErrorCode::invalidStrategyParameters,
                             "Python Strategy parameters require 1 <= "
                             "fast_period < slow_period <= 10000 and quantity "
                             "from 1 through 1000000000.");
  }
  auto serialized = serializeCanonicalJson(parameters);
  if (!serialized.ok()) {
    return serialized.error();
  }
  if (serialized.value().size() > std::size_t{64} * 1024) {
    return makeStrategyError(core::ErrorCode::invalidStrategyParameters,
                             "Python Strategy parameters exceed 64 KiB.");
  }
  return serialized.value();
}

[[nodiscard]] std::string makeRunId() {
  static std::atomic<std::uint64_t> nextId{0};
  std::array<std::uint32_t, 4> entropy{};
  std::random_device random;
  for (auto &part : entropy) {
    part = random();
  }
  const auto serial = nextId.fetch_add(1, std::memory_order_relaxed);
  const std::string result = core::sha256(
      std::to_string(entropy[0]) + ":" + std::to_string(entropy[1]) + ":" +
      std::to_string(entropy[2]) + ":" + std::to_string(entropy[3]) + ":" +
      std::to_string(serial));
  return result.substr(0, 32);
}

[[nodiscard]] core::ErrorCode workerErrorCode(const std::string_view code,
                                              const core::ErrorCode fallback) {
  if (code == "StrategyCommandRejected") {
    return core::ErrorCode::strategyCommandRejected;
  }
  if (code == "StrategyContractViolation") {
    return core::ErrorCode::strategyContractViolation;
  }
  if (code == "StrategyLoadFailed") {
    return core::ErrorCode::strategyLoadFailed;
  }
  if (code == "InvalidStrategyEncoding") {
    return core::ErrorCode::invalidStrategyEncoding;
  }
  if (code == "StrategySourceTooLarge") {
    return core::ErrorCode::strategySourceTooLarge;
  }
  if (code == "StrategySourceContainsNul") {
    return core::ErrorCode::strategySourceContainsNul;
  }
  if (code == "StrategySyntaxError") {
    return core::ErrorCode::strategySyntaxError;
  }
  if (code == "StrategyImportNotAllowed") {
    return core::ErrorCode::strategyImportNotAllowed;
  }
  if (code == "UnsupportedStrategyApiVersion") {
    return core::ErrorCode::unsupportedStrategyApiVersion;
  }
  if (code == "InvalidStrategyParameters") {
    return core::ErrorCode::invalidStrategyParameters;
  }
  if (code == "StrategyConsentRequired") {
    return core::ErrorCode::strategyConsentRequired;
  }
  if (code == "StrategyFactoryFailed") {
    return core::ErrorCode::strategyFactoryFailed;
  }
  if (code == "RuntimeUnavailable") {
    return core::ErrorCode::runtimeUnavailable;
  }
  if (code == "RuntimeIntegrityFailure") {
    return core::ErrorCode::runtimeIntegrityFailure;
  }
  if (code == "strategyRuntimeError") {
    return core::ErrorCode::strategyRuntimeError;
  }
  if (code == "StrategyProtocolViolation") {
    return core::ErrorCode::strategyProtocolViolation;
  }
  if (code == "WorkerHandshakeFailed") {
    return core::ErrorCode::workerHandshakeFailed;
  }
  if (code == "InvalidHistoryRequest") {
    return core::ErrorCode::invalidHistoryRequest;
  }
  if (code == "HistoryUnavailable") {
    return core::ErrorCode::historyUnavailable;
  }
  if (code == "HistoryBudgetExceeded") {
    return core::ErrorCode::historyBudgetExceeded;
  }
  if (code == "FutureDataRequest") {
    return core::ErrorCode::futureDataRequest;
  }
  static_cast<void>(fallback);
  return core::ErrorCode::strategyProtocolViolation;
}

[[nodiscard]] bool isKnownWorkerErrorCode(const std::string_view code) {
  static const std::set<std::string> knownErrorCodes{
      "InvalidStrategyEncoding",
      "InvalidStrategyParameters",
      "StrategyConsentRequired",
      "RuntimeUnavailable",
      "RuntimeIntegrityFailure",
      "StrategyCommandRejected",
      "StrategyContractViolation",
      "StrategyFactoryFailed",
      "StrategyLoadFailed",
      "StrategyProtocolViolation",
      "StrategySourceTooLarge",
      "StrategySyntaxError",
      "StrategyImportNotAllowed",
      "StrategySourceContainsNul",
      "UnsupportedStrategyApiVersion",
      "WorkerHandshakeFailed",
      "InvalidHistoryRequest",
      "HistoryUnavailable",
      "HistoryBudgetExceeded",
      "FutureDataRequest",
      "strategyRuntimeError"};
  return knownErrorCodes.contains(std::string{code});
} // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.

[[nodiscard]] core::Result<void>
appendStartupDiagnostics(std::string &messageText,
                         const nlohmann::json &diagnostics) {
  static const std::set<std::string> keys{
      "omittedTracebackBytes", "sourceColumn", "sourceLine", "traceback"};
  const auto readLocation = [](const nlohmann::json &value,
                               std::optional<std::uint32_t> &output) {
    if (value.is_null()) {
      output.reset();
      return true;
    }
    if (!value.is_number_integer() || value.is_boolean()) {
      return false;
    }
    const auto number = value.get<std::int64_t>();
    if (number <= 0 || number > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    output = static_cast<std::uint32_t>(number);
    return true;
  };
  const auto omittedValue =
      diagnostics.is_object() && diagnostics.contains("omittedTracebackBytes")
          ? diagnostics.at("omittedTracebackBytes")
          : nlohmann::json{};
  if (!hasExactKeys(diagnostics, keys) ||
      !diagnostics.at("traceback").is_string() ||
      !isStrictUtf8(
          diagnostics.at("traceback").get_ref<const std::string &>()) ||
      diagnostics.at("traceback").get_ref<const std::string &>().size() >
          std::size_t{48} * 1024 ||
      (!omittedValue.is_number_unsigned() &&
       !omittedValue.is_number_integer()) ||
      omittedValue.is_boolean() || omittedValue.get<std::int64_t>() < 0) {
    return makeStrategyError(
        core::ErrorCode::strategyProtocolViolation,
        "Python worker returned malformed startup diagnostics.");
  }
  std::optional<std::uint32_t> sourceLine;
  std::optional<std::uint32_t> sourceColumn;
  if (!readLocation(diagnostics.at("sourceLine"), sourceLine) ||
      !readLocation(diagnostics.at("sourceColumn"), sourceColumn)) {
    return makeStrategyError(
        core::ErrorCode::strategyProtocolViolation,
        "Python worker returned an invalid startup source location.");
  }
  if (sourceLine.has_value()) {
    messageText += "\nAt strategy.py:" + std::to_string(*sourceLine);
    if (sourceColumn.has_value()) {
      messageText += ":" + std::to_string(*sourceColumn);
    }
  }
  const auto &tracebackText =
      diagnostics.at("traceback").get_ref<const std::string &>();
  if (!tracebackText.empty()) {
    messageText += "\n" + tracebackText;
  }
  const auto omitted = omittedValue.get<std::uint64_t>();
  if (omitted > 0) {
    messageText +=
        "\n... " + std::to_string(omitted) + " traceback bytes omitted";
  }
  return {};
}

[[nodiscard]] core::Error
decodeWorkerError(const ProtocolMessage &message,
                  const core::ErrorCode fallback,
                  const bool includeStartupDiagnostics = false) {
  static const std::set<std::string> errorKeys{"errorCode", "message"};
  static const std::set<std::string> diagnosticErrorKeys{
      "diagnostics", "errorCode", "message"};
  if (message.messageType != "worker-error" ||
      (!hasExactKeys(message.payload, errorKeys) &&
       !hasExactKeys(message.payload, diagnosticErrorKeys)) ||
      !message.payload.at("errorCode").is_string() ||
      !message.payload.at("message").is_string() ||
      (message.payload.contains("diagnostics") &&
       !message.payload.at("diagnostics").is_object())) {
    return makeStrategyError(
        core::ErrorCode::strategyProtocolViolation,
        "Python worker returned a malformed error message.");
  }
  const auto &code =
      message.payload.at("errorCode").get_ref<const std::string &>();
  if (!isKnownWorkerErrorCode(code)) {
    return makeStrategyError(core::ErrorCode::strategyProtocolViolation,
                             "Python worker returned an unknown error code.");
  }
  auto messageText = message.payload.at("message").get<std::string>();
  if (code == "StrategySyntaxError" || code == "StrategyImportNotAllowed" ||
      code == "StrategySourceContainsNul" ||
      code == "UnsupportedStrategyApiVersion") {
    messageText.insert(0, std::string{code} + ": ");
  }
  if (includeStartupDiagnostics && message.payload.contains("diagnostics")) {
    const auto diagnostics = appendStartupDiagnostics(
        messageText, message.payload.at("diagnostics"));
    if (!diagnostics.ok()) {
      return diagnostics.error();
    }
  }
  return core::makeError(workerErrorCode(code, fallback),
                         std::move(messageText));
}

[[nodiscard]] bool isFiniteJsonValue(const nlohmann::json &value,
                                     const std::size_t depth) {
  if (depth > 4) {
    return false;
  }
  if (value.is_number_float()) {
    return std::isfinite(value.get<double>());
  }
  if (value.is_object()) {
    return std::ranges::all_of(value.items(), [depth](const auto &entry) {
      return isStrictUtf8(entry.key()) && entry.key().size() <= 256 &&
             isFiniteJsonValue(entry.value(), depth + 1);
    });
  }
  if (value.is_array()) {
    return std::ranges::all_of(value, [depth](const auto &entry) {
      return isFiniteJsonValue(entry, depth + 1);
    });
  }
  return value.is_null() || value.is_boolean() || value.is_number_integer() ||
         value.is_number_unsigned() || value.is_string();
}

[[nodiscard]] core::Result<nlohmann::json> makeBarValue(const core::Bar &bar) {
  const auto value = nlohmann::json{
      {"close", bar.close},
      {"high", bar.high},
      {"low", bar.low},
      {"open", bar.open},
      {"timestamp_utc_ms", core::time::toUnixMillis(bar.ts)},
      {"volume", bar.volume},
  };
  const auto serialized = serializeCanonicalJson(value);
  if (!serialized.ok()) {
    return serialized.error();
  }
  return value;
}

[[nodiscard]] core::Result<nlohmann::json>
makeContextValue(const strategy::StrategyContext &context) {
  if (context.symbol.empty() || !isStrictUtf8(context.symbol) ||
      context.positionShares < 0 ||
      context.positionShares > strategy::maximumStrategyQuantityShares ||
      (context.markPriceNanodollars.has_value() &&
       context.markPriceNanodollars.value() <= 0)) {
    return makeStrategyError(core::ErrorCode::strategyContractViolation,
                             "Strategy context has an invalid symbol.");
  }
  nlohmann::json rawBar = nullptr;
  nlohmann::json bars = nlohmann::json::object();
  if (context.bar.has_value()) {
    auto bar = makeBarValue(context.bar.value());
    if (!bar.ok()) {
      return bar.error();
    }
    rawBar = bar.value();
    bars[std::string{context.symbol}] = std::move(bar).value();
  }

  nlohmann::json positions = nlohmann::json::object();
  if (context.positionShares != 0) {
    nlohmann::json markPrice = nullptr;
    if (context.markPriceNanodollars.has_value()) {
      markPrice = static_cast<double>(context.markPriceNanodollars.value()) /
                  nanodollarsPerDollar;
    }
    positions[std::string{context.symbol}] = {
        {"mark_price", std::move(markPrice)},
        {"quantity_shares", context.positionShares},
    };
  }
  nlohmann::json timestamp = nullptr;
  if (context.bar.has_value()) {
    timestamp = core::time::toUnixMillis(context.bar->ts);
  }
  nlohmann::json portfolio{
      {"cash",
       static_cast<double>(context.cashMicrodollars) / microdollarsPerDollar},
      {"equity",
       static_cast<double>(context.equityMicrodollars) / microdollarsPerDollar},
      {"positions", std::move(positions)},
  };
  nlohmann::json indicators = nlohmann::json::object();
  for (const auto &indicator : context.indicatorValues) {
    if (!isValidSmaName(indicator.name) ||
        indicators.contains(indicator.name) ||
        (indicator.value.has_value() &&
         !std::isfinite(indicator.value->value))) {
      return makeStrategyError(
          core::ErrorCode::strategyContractViolation,
          "Engine supplied invalid Strategy indicator values.");
    }
    if (indicator.value.has_value()) {
      indicators[std::string{indicator.name}] = indicator.value->value;
    } else {
      indicators[std::string{indicator.name}] = nullptr;
    }
  }
  nlohmann::json result{{"bar", std::move(rawBar)},
                        {"bars", std::move(bars)},
                        {"indicators", std::move(indicators)},
                        {"portfolio", std::move(portfolio)},
                        {"symbol", std::string{context.symbol}},
                        {"timestamp_utc_ms", std::move(timestamp)}};
  const auto serialized = serializeCanonicalJson(result);
  if (!serialized.ok()) {
    return serialized.error();
  }
  return result;
}

[[nodiscard]] core::Result<nlohmann::json>
makeFillValue(const strategy::StrategyFill &fill) {
  if ((fill.side != strategy::StrategyFillSide::buy &&
       fill.side != strategy::StrategyFillSide::sell) ||
      fill.quantityShares <= 0 ||
      fill.quantityShares > strategy::maximumStrategyQuantityShares ||
      fill.priceNanodollars <= 0 || fill.amountMicrodollars <= 0) {
    return makeStrategyError(
        core::ErrorCode::strategyContractViolation,
        "Engine supplied an invalid Python Strategy fill.");
  }
  const auto side = fill.side == strategy::StrategyFillSide::buy
                        ? std::string{"buy"}
                        : std::string{"sell"};
  const auto value = nlohmann::json{
      {"amount",
       static_cast<double>(fill.amountMicrodollars) / microdollarsPerDollar},
      {"price",
       static_cast<double>(fill.priceNanodollars) / nanodollarsPerDollar},
      {"quantity_shares", fill.quantityShares},
      {"side", side},
      {"timestamp_utc_ms", core::time::toUnixMillis(fill.timestamp)},
  };
  const auto serialized = serializeCanonicalJson(value);
  if (!serialized.ok()) {
    return serialized.error();
  }
  return value;
}

[[nodiscard]] core::Result<void>
validateAndStoreLogs(const nlohmann::json &logs,
                     PythonStrategyDiagnostics &diagnostics,
                     std::size_t &retainedBytes, const std::string_view hook,
                     strategy::StrategyDiagnosticBatch &pending) {
  static const std::set<std::string> logKeys{"fields", "level", "message"};
  if (!logs.is_array()) {
    return makeStrategyError(core::ErrorCode::strategyProtocolViolation,
                             "Python worker returned malformed diagnostics.");
  }
  for (const auto &entry : logs) {
    if (!hasExactKeys(entry, logKeys) || !entry.at("level").is_string() ||
        !entry.at("message").is_string() || !entry.at("fields").is_object()) {
      return makeStrategyError(core::ErrorCode::strategyProtocolViolation,
                               "Python worker returned a malformed log entry.");
    }
    const auto &level = entry.at("level").get_ref<const std::string &>();
    const auto &message = entry.at("message").get_ref<const std::string &>();
    if ((level != "info" && level != "warning" && level != "error") ||
        !isStrictUtf8(message) || message.size() > maximumLogMessageBytes ||
        !isFiniteJsonValue(entry.at("fields"), 0)) {
      return makeStrategyError(
          core::ErrorCode::strategyContractViolation,
          "Python worker returned an invalid structured log.");
    }
    const auto canonical =
        serializeCanonicalJson(nlohmann::json{{"fields", entry.at("fields")},
                                              {"hook", hook},
                                              {"level", level},
                                              {"message", message}});
    if (!canonical.ok()) {
      return canonical.error();
    }
    if (diagnostics.structuredLogs.size() >= maximumStructuredLogEntries ||
        canonical.value().size() > maximumStructuredLogBytes - retainedBytes) {
      ++diagnostics.omittedLogEntries;
      diagnostics.omittedLogBytes += canonical.value().size();
      ++pending.omittedLogEntries;
      pending.omittedLogBytes += canonical.value().size();
      pending.complete = false;
      diagnostics.complete = false;
      continue;
    }
    retainedBytes += canonical.value().size();
    diagnostics.structuredLogs.push_back(entry);
    const auto fields = serializeCanonicalJson(entry.at("fields"));
    if (!fields.ok()) {
      return fields.error();
    }
    pending.records.push_back(strategy::StrategyDiagnostic{
        .kind = strategy::StrategyDiagnosticKind::log,
        .hook = std::string{hook},
        .level = level,
        .errorCode = {},
        .message = message,
        .canonicalFieldsJson = fields.value(),
        .sourceLine = std::nullopt,
        .sourceColumn = std::nullopt,
        .traceback = {},
        .omittedBytes = 0,
        .omittedEntries = 0,
    });
  }
  return {};
}

void recordWorkerOmissions(const std::uint64_t omittedBytes,
                           const std::uint64_t omittedEntries,
                           PythonStrategyDiagnostics &diagnostics,
                           strategy::StrategyDiagnosticBatch &pending) {
  diagnostics.omittedLogBytes += omittedBytes;
  diagnostics.omittedLogEntries += omittedEntries;
  pending.omittedLogBytes += omittedBytes;
  pending.omittedLogEntries += omittedEntries;
  if (omittedBytes > 0 || omittedEntries > 0) {
    diagnostics.complete = false;
    pending.complete = false;
  }
}

[[nodiscard]] core::Result<void> storeWorkerHookFailure(
    const ProtocolMessage &message, const std::string_view expectedHook,
    PythonStrategyDiagnostics &diagnostics, std::size_t &retainedBytes,
    strategy::StrategyDiagnosticBatch &pending) {
  static const std::set<std::string> errorKeys{"errorCode", "message"};
  static const std::set<std::string> diagnosticErrorKeys{
      "diagnostics", "errorCode", "message"};
  if ((!hasExactKeys(message.payload, errorKeys) &&
       !hasExactKeys(message.payload, diagnosticErrorKeys)) ||
      !message.payload.at("errorCode").is_string() ||
      !message.payload.at("message").is_string() ||
      !isKnownWorkerErrorCode(
          message.payload.at("errorCode").get_ref<const std::string &>()) ||
      !isStrictUtf8(
          message.payload.at("message").get_ref<const std::string &>()) ||
      message.payload.at("message").get_ref<const std::string &>().size() >
          std::size_t{64} * 1024) {
    return makeStrategyError(
        core::ErrorCode::strategyProtocolViolation,
        "Python worker returned a malformed error envelope.");
  }
  if (!message.payload.contains("diagnostics")) {
    storeTerminalDiagnostic(
        message.payload.at("errorCode").get_ref<const std::string &>(),
        message.payload.at("message").get_ref<const std::string &>(),
        expectedHook, std::nullopt, std::nullopt, {}, 0, 0, diagnostics,
        pending);
    return {};
  }
  const auto &value = message.payload.at("diagnostics");
  static const std::set<std::string> keys{"hook",
                                          "logs",
                                          "omittedLogBytes",
                                          "omittedLogEntries",
                                          "omittedTracebackBytes",
                                          "sourceColumn",
                                          "sourceLine",
                                          "traceback"};
  const auto readCount = [](const nlohmann::json &item, std::uint64_t &output) {
    if ((!item.is_number_unsigned() && !item.is_number_integer()) ||
        item.is_boolean()) {
      return false;
    }
    const auto number = item.get<std::int64_t>();
    if (number < 0) {
      return false;
    }
    output = static_cast<std::uint64_t>(number);
    return true;
  };
  const auto readLocation = [](const nlohmann::json &item,
                               std::optional<std::uint32_t> &output) {
    if (item.is_null()) {
      output.reset();
      return true;
    }
    if (!item.is_number_integer() || item.is_boolean()) {
      return false;
    }
    const auto number = item.get<std::int64_t>();
    if (number <= 0 || number > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    output = static_cast<std::uint32_t>(number);
    return true;
  };
  if (!hasExactKeys(value, keys) || !value.at("hook").is_string() ||
      value.at("hook").get<std::string>() != expectedHook ||
      !value.at("traceback").is_string() ||
      !isStrictUtf8(value.at("traceback").get_ref<const std::string &>()) ||
      value.at("traceback").get_ref<const std::string &>().size() >
          std::size_t{64} * 1024) {
    return makeStrategyError(
        core::ErrorCode::strategyProtocolViolation,
        "Python worker returned malformed hook diagnostics.");
  }
  std::uint64_t omittedBytes = 0;
  std::uint64_t omittedEntries = 0;
  std::uint64_t omittedTracebackBytes = 0;
  std::optional<std::uint32_t> sourceLine;
  std::optional<std::uint32_t> sourceColumn;
  if (!readCount(value.at("omittedLogBytes"), omittedBytes) ||
      !readCount(value.at("omittedLogEntries"), omittedEntries) ||
      !readCount(value.at("omittedTracebackBytes"), omittedTracebackBytes) ||
      !readLocation(value.at("sourceLine"), sourceLine) ||
      !readLocation(value.at("sourceColumn"), sourceColumn)) {
    return makeStrategyError(
        core::ErrorCode::strategyProtocolViolation,
        "Python worker returned invalid diagnostic counts or source location.");
  }
  const auto logs = validateAndStoreLogs(value.at("logs"), diagnostics,
                                         retainedBytes, expectedHook, pending);
  if (!logs.ok()) {
    return logs.error();
  }
  recordWorkerOmissions(omittedBytes, omittedEntries, diagnostics, pending);
  storeTerminalDiagnostic(
      message.payload.at("errorCode").get_ref<const std::string &>(),
      message.payload.at("message").get_ref<const std::string &>(),
      expectedHook, sourceLine, sourceColumn,
      value.at("traceback").get_ref<const std::string &>(),
      omittedTracebackBytes, omittedEntries, diagnostics, pending);
  return {};
}

[[nodiscard]] core::Result<void>
storeCallbackOmissions(const nlohmann::json &payload,
                       PythonStrategyDiagnostics &diagnostics,
                       strategy::StrategyDiagnosticBatch &pending) {
  const auto readCount = [](const nlohmann::json &item, std::uint64_t &output) {
    if ((!item.is_number_unsigned() && !item.is_number_integer()) ||
        item.is_boolean()) {
      return false;
    }
    const auto number = item.get<std::int64_t>();
    if (number < 0) {
      return false;
    }
    output = static_cast<std::uint64_t>(number);
    return true;
  };
  std::uint64_t omittedBytes = 0;
  std::uint64_t omittedEntries = 0;
  if (!readCount(payload.at("omittedLogBytes"), omittedBytes) ||
      !readCount(payload.at("omittedLogEntries"), omittedEntries)) {
    return makeStrategyError(
        core::ErrorCode::strategyProtocolViolation,
        "Python worker returned invalid diagnostic counts.");
  }
  recordWorkerOmissions(omittedBytes, omittedEntries, diagnostics, pending);
  return {};
}

[[nodiscard]] core::Result<strategy::StrategyCommandBatch>
decodeCommands(const nlohmann::json &commands,
               const std::string_view expectedSymbol,
               const bool commandsAllowed) {
  if (!commands.is_array() ||
      commands.size() >
          (commandsAllowed ? strategy::maximumCommandsPerMarketCallback : 0U)) {
    return makeStrategyError(
        core::ErrorCode::strategyCommandRejected,
        "Python Strategy returned an unsupported command batch.");
  }
  strategy::StrategyCommandBatch result;
  result.reserve(commands.size());
  for (const auto &command : commands) {
    static const std::set<std::string> commandKeys{"kind", "quantity_shares",
                                                   "symbol"};
    if (!hasExactKeys(command, commandKeys) ||
        !command.at("kind").is_string() || !command.at("symbol").is_string() ||
        !command.at("quantity_shares").is_number_integer() ||
        command.at("quantity_shares").is_boolean()) {
      return makeStrategyError(core::ErrorCode::strategyCommandRejected,
                               "Python Strategy returned a malformed command.");
    }
    const auto &symbol = command.at("symbol").get_ref<const std::string &>();
    if (symbol != expectedSymbol) {
      return makeStrategyError(
          core::ErrorCode::strategyCommandRejected,
          "Python Strategy command symbol does not match the run.");
    }
    std::int64_t quantity = 0;
    const auto quantityValid =
        readWholeInteger(command.at("quantity_shares"), 0,
                         strategy::maximumStrategyQuantityShares, quantity);
    if (!quantityValid) {
      return makeStrategyError(core::ErrorCode::strategyCommandRejected,
                               "Python Strategy command quantity is invalid.");
    }
    const auto &kind = command.at("kind").get_ref<const std::string &>();
    if (kind == "buy_market" && quantity > 0) {
      result.push_back({.kind = strategy::StrategyCommandKind::buyMarket,
                        .symbol = symbol,
                        .quantityShares = quantity});
    } else if (kind == "close_position" && quantity == 0) {
      result.push_back({.kind = strategy::StrategyCommandKind::closePosition,
                        .symbol = symbol,
                        .quantityShares = 0});
    } else {
      return makeStrategyError(
          core::ErrorCode::strategyCommandRejected,
          "Python Strategy command kind or quantity is invalid.");
    }
  }
  return result;
}

} // namespace

struct PythonWorkerStrategy::Impl final {
  ManagedWorkerProcess process;
  ReadOnlySharedMemory sourceMemory;
  ReadOnlySharedMemory historyMemory;
  strategy::StrategyRequirements strategyRequirements;
  std::string runId;
  std::uint64_t inputSequence = 0;
  std::uint64_t outputSequence = 0;
  std::size_t retainedLogBytes = 0;
  std::size_t maxHistoryBars = 0;
  PythonStrategyDiagnostics diagnosticData;
  strategy::StrategyDiagnosticBatch pendingDiagnostics;
  bool streamDiagnosticsCollected = false;
  bool stopped = false;

  [[nodiscard]] core::Result<void>
  send(std::string messageType, nlohmann::json payload,
       const std::chrono::milliseconds timeout,
       const core::ErrorCode timeoutCode,
       const core::CancellationToken &cancellation = {}) {
    auto sent = process.sendMessage(
        ProtocolMessage{.protocolVersion = currentProtocolVersion,
                        .runId = runId,
                        .sequence = inputSequence,
                        .messageType = std::move(messageType),
                        .payload = std::move(payload)},
        timeout, timeoutCode, cancellation);
    if (sent.ok()) {
      ++inputSequence;
    }
    return sent;
  }

  [[nodiscard]] core::Result<ProtocolMessage>
  receive(const std::chrono::milliseconds timeout,
          const core::ErrorCode timeoutCode,
          const core::CancellationToken &cancellation = {}) {
    auto received = process.receiveMessage(runId, outputSequence, timeout,
                                           timeoutCode, cancellation);
    if (received.ok()) {
      ++outputSequence;
    }
    return received;
  }

  [[nodiscard]] core::Result<ProtocolMessage>
  exchange(std::string messageType, nlohmann::json payload,
           const std::chrono::milliseconds timeout,
           const core::ErrorCode timeoutCode,
           const core::CancellationToken &cancellation = {}) {
    const auto sent = send(std::move(messageType), std::move(payload), timeout,
                           timeoutCode, cancellation);
    if (!sent.ok()) {
      return sent.error();
    }
    return receive(timeout, timeoutCode, cancellation);
  }

  [[nodiscard]] core::Result<std::string>
  validateBootstrap(const bool verifyRuntime,
                    const core::CancellationToken &cancellation) {
    const auto bootstrapReady =
        receive(bootstrapDeadline, core::ErrorCode::workerBootstrapTimeout,
                cancellation);
    static const std::set<std::string> readyKeys{"protocolVersion",
                                                 "pythonVersion"};
    if (!bootstrapReady.ok()) {
      terminate();
      return bootstrapReady.error();
    }
    if (bootstrapReady.value().messageType != "bootstrap-ready" ||
        !hasExactKeys(bootstrapReady.value().payload, readyKeys) ||
        !bootstrapReady.value()
             .payload.at("protocolVersion")
             .is_number_integer() ||
        bootstrapReady.value().payload.at("protocolVersion").get<int>() !=
            static_cast<int>(currentProtocolVersion) ||
        !bootstrapReady.value().payload.at("pythonVersion").is_string()) {
      terminate();
      return makeStrategyError(core::ErrorCode::workerStartFailed,
                               "Python worker bootstrap identity is invalid.");
    }
    const auto workerPythonVersion =
        bootstrapReady.value().payload.at("pythonVersion").get<std::string>();
    if (verifyRuntime && workerPythonVersion != "3.14.9") {
      terminate();
      return makeStrategyError(
          core::ErrorCode::runtimeIntegrityFailure,
          "Managed Python interpreter version changed after verification.");
    }
    return workerPythonVersion;
  }

  [[nodiscard]] core::Result<void>
  validateHandshake(const std::string_view workerPythonVersion,
                    const core::CancellationToken &cancellation) {
    auto handshake =
        exchange("handshake",
                 nlohmann::json{{"protocolVersion", currentProtocolVersion},
                                {"stockbtApiVersion", "1"}},
                 handshakeDeadline, core::ErrorCode::workerHandshakeTimeout,
                 cancellation);
    if (!handshake.ok()) {
      terminate();
      return handshake.error();
    }
    static const std::set<std::string> handshakeKeys{"pythonVersion",
                                                     "stockbtApiVersion"};
    if (handshake.value().messageType == "worker-error") {
      const auto error = decodeWorkerError(
          handshake.value(), core::ErrorCode::workerHandshakeFailed);
      terminate();
      return error;
    }
    if (handshake.value().messageType != "handshake-complete" ||
        !hasExactKeys(handshake.value().payload, handshakeKeys) ||
        handshake.value().payload.at("stockbtApiVersion") != "1" ||
        handshake.value().payload.at("pythonVersion") != workerPythonVersion) {
      terminate();
      return makeStrategyError(core::ErrorCode::workerHandshakeFailed,
                               "Python worker handshake response is invalid.");
    }
    return {};
  }

  [[nodiscard]] core::Result<void>
  createStrategy(nlohmann::json parameters,
                 const core::CancellationToken &cancellation) {
    auto createdStrategy = exchange(
        "create-strategy",
        nlohmann::json{{"parameters", std::move(parameters)}}, factoryDeadline,
        core::ErrorCode::strategyFactoryTimeout, cancellation);
    if (!createdStrategy.ok()) {
      terminate();
      return createdStrategy.error();
    }
    if (createdStrategy.value().messageType == "worker-error") {
      const auto error =
          decodeWorkerError(createdStrategy.value(),
                            core::ErrorCode::strategyFactoryFailed, true);
      terminate();
      return error;
    }
    static const std::set<std::string> strategyKeys{"indicators",
                                                    "maxHistoryBars"};
    std::int64_t maxHistoryBarCount = 0;
    if (createdStrategy.value().messageType != "strategy-loaded" ||
        !hasExactKeys(createdStrategy.value().payload, strategyKeys) ||
        !readWholeInteger(createdStrategy.value().payload.at("maxHistoryBars"),
                          1, 10'000, maxHistoryBarCount) ||
        !createdStrategy.value().payload.at("indicators").is_array()) {
      terminate();
      return makeStrategyError(
          core::ErrorCode::strategyContractViolation,
          "Python worker returned an invalid strategy contract.");
    }
    this->maxHistoryBars = static_cast<std::size_t>(maxHistoryBarCount);
    strategyRequirements.maxHistoryBars = this->maxHistoryBars;
    std::set<std::string> indicatorNames;
    for (const auto &indicator :
         createdStrategy.value().payload.at("indicators")) {
      static const std::set<std::string> indicatorKeys{"kind", "name",
                                                       "period"};
      std::int64_t period = 0;
      if (!hasExactKeys(indicator, indicatorKeys) ||
          !indicator.at("kind").is_string() || indicator.at("kind") != "SMA" ||
          !indicator.at("name").is_string() ||
          indicator.at("period").is_boolean() ||
          !readWholeInteger(indicator.at("period"), 1, 10'000, period)) {
        terminate();
        return makeStrategyError(
            core::ErrorCode::strategyContractViolation,
            "Python Strategy supports only valid SMA registrations.");
      }
      const auto name = indicator.at("name").get<std::string>();
      if (!isStrictUtf8(name) || !isValidSmaName(name) ||
          !indicatorNames.insert(name).second || period > maxHistoryBarCount) {
        terminate();
        return makeStrategyError(core::ErrorCode::strategyContractViolation,
                                 period > maxHistoryBarCount
                                     ? "Python Strategy history budget is "
                                       "smaller than an SMA period."
                                     : "Python Strategy SMA names must be "
                                       "unique ASCII identifiers.");
      }
      strategyRequirements.indicators.push_back(
          {.name = name,
           .definition = {.kind = indicators::IndicatorKind::sma,
                          .period = static_cast<std::int32_t>(period),
                          .secondaryPeriod = 0,
                          .signalPeriod = 0,
                          .output = indicators::IndicatorOutput::value,
                          .field = indicators::BarField::close}});
    }
    return {};
  }

  [[nodiscard]] core::Result<void>
  loadWorkerModule(const core::CancellationToken &cancellation) {
    auto loadedModule = exchange(
        "load-module",
        nlohmann::json{{"sourceSha256", sourceMemory.sha256Digest()},
                       {"stockbtApiVersion", "1"}},
        moduleDeadline, core::ErrorCode::strategyLoadTimeout, cancellation);
    if (!loadedModule.ok()) {
      terminate();
      return loadedModule.error();
    }
    if (loadedModule.value().messageType == "worker-error") {
      const auto error = decodeWorkerError(
          loadedModule.value(), core::ErrorCode::strategyLoadFailed, true);
      terminate();
      return error;
    }
    static const std::set<std::string> moduleKeys{"stockbtApiVersion"};
    if (loadedModule.value().messageType != "module-loaded" ||
        !hasExactKeys(loadedModule.value().payload, moduleKeys) ||
        loadedModule.value().payload.at("stockbtApiVersion") != "1") {
      terminate();
      return makeStrategyError(core::ErrorCode::strategyLoadFailed,
                               "Python worker did not confirm module loading.");
    }
    sourceMemory = ReadOnlySharedMemory{};
    return {};
  }

  [[nodiscard]] core::Result<void>
  captureSuccessfulHook(const nlohmann::json &payload,
                        const std::string_view hook) {
    auto logs =
        validateAndStoreLogs(payload.at("logs"), diagnosticData,
                             retainedLogBytes, hook, pendingDiagnostics);
    if (!logs.ok()) {
      return logs.error();
    }
    return storeCallbackOmissions(payload, diagnosticData, pendingDiagnostics);
  }

  [[nodiscard]] core::Result<void>
  captureFailedHook(const ProtocolMessage &message,
                    const std::string_view hook) {
    return storeWorkerHookFailure(message, hook, diagnosticData,
                                  retainedLogBytes, pendingDiagnostics);
  }

  void captureCallbackFailure(const core::Error &error,
                              const std::string_view hook,
                              const bool diagnosticsMayBeIncomplete = false) {
    storeTerminalDiagnostic(strategyDiagnosticCode(error.code), error.message,
                            hook, std::nullopt, std::nullopt, {}, 0, 0,
                            diagnosticData, pendingDiagnostics);
    if (diagnosticsMayBeIncomplete) {
      diagnosticData.complete = false;
      pendingDiagnostics.complete = false;
    }
  }

  [[nodiscard]] core::Result<void>
  seedHistory(const std::span<const core::Bar> bars,
              const core::CancellationToken &cancellation) {
    if (bars.size() > maxHistoryBars) {
      return makeStrategyError(
          core::ErrorCode::invalidHistoryRequest,
          "Python Strategy warm-up history exceeds its declared budget.");
    }
    const auto encodedHistory = encodeHistoryBars(bars);
    const auto response = exchange(
        "history-seed",
        nlohmann::json{{"barCount", bars.size()},
                       {"historySha256", core::sha256(encodedHistory)}},
        hookDeadline, core::ErrorCode::strategyHookTimeout, cancellation);
    if (!response.ok()) {
      return response.error();
    }
    if (response.value().messageType == "worker-error") {
      return decodeWorkerError(response.value(),
                               core::ErrorCode::strategyProtocolViolation);
    }
    if (response.value().messageType != "history-seeded" ||
        !response.value().payload.is_object() ||
        !response.value().payload.empty()) {
      return makeStrategyError(core::ErrorCode::strategyProtocolViolation,
                               "Python worker rejected its history seed.");
    }
    return {};
  }

  void terminate(
      const std::chrono::milliseconds grace = std::chrono::milliseconds{100}) {
    if (process.isRunning()) {
      static_cast<void>(process.terminateProcessTree(grace));
    }
    stopped = true;
  }

  void stopAfterCallbackError(const core::Error &error) {
    if (error.code == core::ErrorCode::cancelled ||
        error.code == core::ErrorCode::strategyHookTimeout) {
      static_cast<void>(shutdown());
    } else {
      terminate(shutdownDeadline);
    }
  }

  [[nodiscard]] core::Result<void> shutdown() {
    if (stopped || !process.isRunning()) {
      stopped = true;
      return {};
    }
    const auto deadline = std::chrono::steady_clock::now() + shutdownDeadline;
    const auto sent =
        send("shutdown", nlohmann::json::object(), shutdownDeadline,
             core::ErrorCode::workerHandshakeTimeout);
    if (sent.ok()) {
      while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        const auto response =
            receive(remaining, core::ErrorCode::workerHandshakeTimeout);
        if (!response.ok()) {
          break;
        }
        if (response.value().messageType == "shutdown-complete") {
          const auto remainingForExit =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  deadline - std::chrono::steady_clock::now());
          if (remainingForExit.count() > 0 &&
              process.waitForExit(remainingForExit).ok()) {
            stopped = true;
            return {};
          }
          break;
        }
        if (response.value().messageType != "callback-result" &&
            response.value().messageType != "worker-error") {
          const auto error = makeStrategyError(
              core::ErrorCode::strategyProtocolViolation,
              "Python worker returned an invalid shutdown response.");
          terminate(std::chrono::milliseconds{0});
          return error;
        }
        if (response.value().messageType == "worker-error") {
          const auto error = decodeWorkerError(
              response.value(), core::ErrorCode::workerHandshakeFailed);
          terminate(std::chrono::milliseconds{0});
          return error;
        }
      }
    }
    const auto error = makeStrategyError(
        core::ErrorCode::workerHandshakeTimeout,
        "Python worker did not complete its bounded shutdown.");
    terminate(std::chrono::milliseconds{0});
    return error;
  }

  void shutdownNoexcept() noexcept {
    try {
      static_cast<void>(shutdown());
    } catch (...) {
      terminate(std::chrono::milliseconds{0});
    } // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.
  }

  ~Impl() noexcept { shutdownNoexcept(); }

  Impl() = default;
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  Impl(Impl &&) = delete;
  Impl &operator=(Impl &&) = delete;
};

PythonWorkerStrategy::PythonWorkerStrategy(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PythonWorkerStrategy::~PythonWorkerStrategy() = default;

core::Result<std::unique_ptr<strategy::Strategy>>
PythonWorkerStrategy::create(const RuntimeProfile &verifiedRuntime,
                             const std::filesystem::path &workerBootstrapPath,
                             std::string source, nlohmann::json parameters,
                             const bool sourceConsentGranted,
                             const core::CancellationToken &cancellation,
                             const std::span<const core::Bar> warmupHistory) {
  if (cancellation.isCancellationRequested()) {
    return makeStrategyError(core::ErrorCode::cancelled,
                             "Python Strategy startup was cancelled.");
  }
  const auto runtime = verifyRuntimeProfile(verifiedRuntime.root);
  if (!runtime.ok()) {
    return runtime.error();
  }
  if (runtime.value().profileId != verifiedRuntime.profileId ||
      runtime.value().manifestHash != verifiedRuntime.manifestHash ||
      runtime.value().root != verifiedRuntime.root ||
      runtime.value().pythonExecutablePath !=
          verifiedRuntime.pythonExecutablePath ||
      runtime.value().timezoneDatabasePath !=
          verifiedRuntime.timezoneDatabasePath ||
      runtime.value().pythonVersion != verifiedRuntime.pythonVersion ||
      runtime.value().pythonAbi != verifiedRuntime.pythonAbi ||
      runtime.value().stockbtApiVersion != verifiedRuntime.stockbtApiVersion) {
    return makeStrategyError(
        core::ErrorCode::runtimeIntegrityFailure,
        "Managed Python runtime changed after verification.");
  }
  return createInternal(
      runtime.value().pythonExecutablePath, workerBootstrapPath,
      std::move(source), std::move(parameters), sourceConsentGranted, true,
      false, cancellation, warmupHistory, runtime.value().timezoneDatabasePath);
}

#if defined(BTE_ENABLE_TEST_HOOKS)
core::Result<std::unique_ptr<strategy::Strategy>>
PythonWorkerStrategy::createForTesting(
    const std::filesystem::path &pythonExecutable,
    const std::filesystem::path &workerBootstrapPath, std::string source,
    nlohmann::json parameters, const bool sourceConsentGranted,
    const bool skipNumpySeedForTest,
    const core::CancellationToken &cancellation,
    const std::span<const core::Bar> warmupHistory,
    std::filesystem::path timezoneDatabasePath) {
  return createInternal(
      pythonExecutable, workerBootstrapPath, std::move(source),
      std::move(parameters), sourceConsentGranted, false, skipNumpySeedForTest,
      cancellation, warmupHistory, std::move(timezoneDatabasePath));
} // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.
#endif

core::Result<std::unique_ptr<strategy::Strategy>>
PythonWorkerStrategy::createInternal(
    const std::filesystem::path &pythonExecutable,
    const std::filesystem::path &workerBootstrapPath, std::string source,
    nlohmann::json parameters, const bool sourceConsentGranted,
    const bool verifyRuntime, const bool skipNumpySeedForTest,
    const core::CancellationToken &cancellation,
    const std::span<const core::Bar> warmupHistory,
    std::filesystem::path timezoneDatabasePath) {
  if (cancellation.isCancellationRequested()) {
    return makeStrategyError(core::ErrorCode::cancelled,
                             "Python Strategy startup was cancelled.");
  }
  if (!sourceConsentGranted) {
    return makeStrategyError(
        core::ErrorCode::strategyConsentRequired,
        "Consent is required before Python Strategy code runs.");
  }
  auto normalizedSource = normalizeSource(std::move(source));
  if (!normalizedSource.ok()) {
    return normalizedSource.error();
  }
  const auto canonicalParameters = validateParameters(parameters);
  if (!canonicalParameters.ok()) {
    return canonicalParameters.error();
  }
  std::error_code filesystemError;
  const auto executable =
      std::filesystem::canonical(pythonExecutable, filesystemError);
  if (filesystemError || !std::filesystem::is_regular_file(executable)) {
    return makeStrategyError(core::ErrorCode::runtimeUnavailable,
                             "The managed Python interpreter is missing.");
  }
  const auto bootstrap =
      std::filesystem::canonical(workerBootstrapPath, filesystemError);
  if (filesystemError || !std::filesystem::is_regular_file(bootstrap)) {
    return makeStrategyError(
        core::ErrorCode::workerStartFailed,
        "The project-owned Python worker bootstrap is missing.");
  }
  auto sharedMemory = ReadOnlySharedMemory::create(normalizedSource.value());
  if (!sharedMemory.ok()) {
    return sharedMemory.error();
  }
  auto sourceLease = std::move(sharedMemory).value();
  if (warmupHistory.size() > 10'000) {
    return makeStrategyError(
        core::ErrorCode::invalidArgument,
        "Python Strategy warm-up history exceeds 10000 bars.");
  }
  ReadOnlySharedMemory historyLease;
  if (!warmupHistory.empty()) {
    const auto historyBytes = encodeHistoryBars(warmupHistory);
    auto historyMemory = ReadOnlySharedMemory::create(historyBytes);
    if (!historyMemory.ok()) {
      return historyMemory.error();
    }
    historyLease = std::move(historyMemory).value();
  }
  std::string runId;
  try {
    runId = makeRunId();
  } catch (const std::exception &) {
    return makeStrategyError(
        core::ErrorCode::workerStartFailed,
        "Could not create a unique Python worker run identity.");
  } // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.
  std::vector<std::string> arguments{"-P", "-s", "-B", bootstrap.string(),
                                     "--run-id=" + runId};
  if (skipNumpySeedForTest) {
    arguments.emplace_back("--test-only-skip-numpy-seed");
  }
  if (verifyRuntime) {
    arguments.emplace_back("--require-managed-stockbt");
  }
  auto workerResult = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = executable,
      .arguments = std::move(arguments),
      .timezoneDatabasePath = std::move(timezoneDatabasePath),
      .readOnlyDataDescriptor = sourceLease.descriptor(),
      .readOnlyDataLength = sourceLease.size(),
      .readOnlyDataSha256 = sourceLease.sha256Digest(),
      .readOnlyHistoryDescriptor = historyLease.descriptor(),
      .readOnlyHistoryLength = historyLease.size(),
      .readOnlyHistorySha256 = historyLease.sha256Digest(),
  });
  if (!workerResult.ok()) {
    return workerResult.error();
  }
  auto impl = std::make_unique<Impl>();
  impl->process = std::move(workerResult).value();
  impl->sourceMemory = std::move(sourceLease);
  impl->historyMemory = std::move(historyLease);
  impl->runId = std::move(runId);

  if (cancellation.isCancellationRequested()) {
    impl->terminate(std::chrono::milliseconds{0});
    return makeStrategyError(core::ErrorCode::cancelled,
                             "Python Strategy startup was cancelled.");
  }

  auto bootstrapVersion = impl->validateBootstrap(verifyRuntime, cancellation);
  if (!bootstrapVersion.ok()) {
    return bootstrapVersion.error();
  }
  const auto workerPythonVersion = std::move(bootstrapVersion).value();

  const auto handshake =
      impl->validateHandshake(workerPythonVersion, cancellation);
  if (!handshake.ok()) {
    return handshake.error();
  }

  const auto moduleLoaded = impl->loadWorkerModule(cancellation);
  if (!moduleLoaded.ok()) {
    return moduleLoaded.error();
  }

  const auto strategyCreated =
      impl->createStrategy(std::move(parameters), cancellation);
  if (!strategyCreated.ok()) {
    return strategyCreated.error();
  }
  auto strategyInstance =
      std::make_unique<PythonWorkerStrategy>(std::move(impl));
  return std::unique_ptr<strategy::Strategy>{std::move(strategyInstance)};
}

core::Result<void>
PythonWorkerStrategy::onStart(const strategy::StrategyContext &context,
                              const core::CancellationToken &cancellation) {
  const auto seeded = impl_->seedHistory(context.warmupHistory, cancellation);
  if (!seeded.ok()) {
    impl_->captureCallbackFailure(seeded.error(), "warmup");
    impl_->stopAfterCallbackError(seeded.error());
    return seeded.error();
  }
  auto contextValue = makeContextValue(context);
  if (!contextValue.ok()) {
    impl_->captureCallbackFailure(contextValue.error(), "on_start");
    impl_->stopAfterCallbackError(contextValue.error());
    return contextValue.error();
  }
  const auto response = impl_->exchange(
      "callback",
      nlohmann::json{{"context", std::move(contextValue).value()},
                     {"hook", "on_start"}},
      hookDeadline, core::ErrorCode::strategyHookTimeout, cancellation);
  if (!response.ok()) {
    impl_->captureCallbackFailure(response.error(), "on_start", true);
    impl_->stopAfterCallbackError(response.error());
    return response.error();
  }
  if (response.value().messageType == "worker-error") {
    const auto captured =
        impl_->captureFailedHook(response.value(), "on_start");
    if (!captured.ok()) {
      impl_->captureCallbackFailure(captured.error(), "on_start", true);
      impl_->terminate(shutdownDeadline);
      return captured.error();
    }
    const auto error = decodeWorkerError(response.value(),
                                         core::ErrorCode::strategyRuntimeError);
    impl_->terminate(shutdownDeadline);
    return error;
  }
  static const std::set<std::string> callbackKeys{
      "commands", "logs", "omittedLogBytes", "omittedLogEntries"};
  if (response.value().messageType != "callback-result" ||
      !hasExactKeys(response.value().payload, callbackKeys)) {
    const auto error = makeStrategyError(
        core::ErrorCode::strategyProtocolViolation,
        "Python worker returned a malformed callback response.");
    impl_->captureCallbackFailure(error, "on_start", true);
    impl_->terminate(shutdownDeadline);
    return error;
  }
  const auto captured =
      impl_->captureSuccessfulHook(response.value().payload, "on_start");
  auto commands = decodeCommands(response.value().payload.at("commands"),
                                 context.symbol, false);
  if (!captured.ok() || !commands.ok()) {
    const auto &error = !captured.ok() ? captured.error() : commands.error();
    impl_->captureCallbackFailure(error, "on_start", !captured.ok());
    impl_->terminate(shutdownDeadline);
    return error;
  }
  return {};
}

const strategy::StrategyRequirements &
PythonWorkerStrategy::requirements() const noexcept {
  return impl_->strategyRequirements;
}

core::Result<strategy::StrategyCommandBatch>
PythonWorkerStrategy::onMarket(const strategy::StrategyContext &context,
                               const core::CancellationToken &cancellation) {
  if (!context.bar.has_value()) {
    const auto error =
        makeStrategyError(core::ErrorCode::strategyContractViolation,
                          "Python on_market requires a current actual bar.");
    impl_->captureCallbackFailure(error, "on_market");
    impl_->stopAfterCallbackError(error);
    return error;
  }
  auto contextValue = makeContextValue(context);
  if (!contextValue.ok()) {
    impl_->captureCallbackFailure(contextValue.error(), "on_market");
    impl_->stopAfterCallbackError(contextValue.error());
    return contextValue.error();
  }
  const auto response = impl_->exchange(
      "callback",
      nlohmann::json{{"context", std::move(contextValue).value()},
                     {"hook", "on_market"}},
      hookDeadline, core::ErrorCode::strategyHookTimeout, cancellation);
  if (!response.ok()) {
    impl_->captureCallbackFailure(response.error(), "on_market", true);
    impl_->stopAfterCallbackError(response.error());
    return response.error();
  }
  if (response.value().messageType == "worker-error") {
    const auto captured =
        impl_->captureFailedHook(response.value(), "on_market");
    if (!captured.ok()) {
      impl_->captureCallbackFailure(captured.error(), "on_market", true);
      impl_->terminate(shutdownDeadline);
      return captured.error();
    }
    const auto error = decodeWorkerError(response.value(),
                                         core::ErrorCode::strategyRuntimeError);
    impl_->terminate(shutdownDeadline);
    return error;
  }
  static const std::set<std::string> callbackKeys{
      "commands", "logs", "omittedLogBytes", "omittedLogEntries"};
  if (response.value().messageType != "callback-result" ||
      !hasExactKeys(response.value().payload, callbackKeys)) {
    const auto error = makeStrategyError(
        core::ErrorCode::strategyProtocolViolation,
        "Python worker returned a malformed callback response.");
    impl_->captureCallbackFailure(error, "on_market", true);
    impl_->terminate(shutdownDeadline);
    return error;
  } // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.
  const auto captured =
      impl_->captureSuccessfulHook(response.value().payload, "on_market");
  auto commands = decodeCommands(response.value().payload.at("commands"),
                                 context.symbol, true);
  if (!captured.ok() || !commands.ok()) {
    const auto &error = !captured.ok() ? captured.error() : commands.error();
    impl_->captureCallbackFailure(error, "on_market", !captured.ok());
    impl_->terminate(shutdownDeadline);
    return error;
  }
  return commands;
}

core::Result<void>
PythonWorkerStrategy::onFill(const strategy::StrategyFill &fill,
                             const strategy::StrategyContext &context,
                             const core::CancellationToken &cancellation) {
  auto contextValue = makeContextValue(context);
  if (!contextValue.ok()) {
    impl_->captureCallbackFailure(contextValue.error(), "on_fill");
    impl_->stopAfterCallbackError(contextValue.error());
    return contextValue.error();
  }
  auto fillValue = makeFillValue(fill);
  if (!fillValue.ok()) {
    impl_->captureCallbackFailure(fillValue.error(), "on_fill");
    impl_->stopAfterCallbackError(fillValue.error());
    return fillValue.error();
  }
  const auto response = impl_->exchange(
      "callback",
      nlohmann::json{{"context", std::move(contextValue).value()},
                     {"fill", std::move(fillValue).value()},
                     {"hook", "on_fill"}},
      hookDeadline, core::ErrorCode::strategyHookTimeout, cancellation);
  if (!response.ok()) {
    impl_->captureCallbackFailure(response.error(), "on_fill", true);
    impl_->stopAfterCallbackError(response.error());
    return response.error();
  }
  if (response.value().messageType == "worker-error") {
    const auto captured = impl_->captureFailedHook(response.value(), "on_fill");
    if (!captured.ok()) {
      impl_->captureCallbackFailure(captured.error(), "on_fill", true);
      impl_->terminate(shutdownDeadline);
      return captured.error();
    }
    const auto error = decodeWorkerError(response.value(),
                                         core::ErrorCode::strategyRuntimeError);
    impl_->terminate(shutdownDeadline);
    return error;
  }
  static const std::set<std::string> callbackKeys{
      "commands", "logs", "omittedLogBytes", "omittedLogEntries"};
  if (response.value().messageType != "callback-result" ||
      !hasExactKeys(response.value().payload, callbackKeys)) {
    const auto error = makeStrategyError(
        core::ErrorCode::strategyProtocolViolation,
        "Python worker returned a malformed callback response.");
    impl_->captureCallbackFailure(error, "on_fill", true);
    impl_->terminate(shutdownDeadline);
    return error;
  }
  const auto captured =
      impl_->captureSuccessfulHook(response.value().payload, "on_fill");
  auto commands = decodeCommands(response.value().payload.at("commands"),
                                 context.symbol, false);
  if (!captured.ok() || !commands.ok()) {
    const auto &error = !captured.ok() ? captured.error() : commands.error();
    impl_->captureCallbackFailure(error, "on_fill", !captured.ok());
    impl_->terminate(shutdownDeadline);
    return error;
  }
  return {};
}

core::Result<void>
PythonWorkerStrategy::onFinish(const strategy::StrategyContext &context,
                               const core::CancellationToken &cancellation) {
  auto contextValue = makeContextValue(context);
  if (!contextValue.ok()) {
    impl_->captureCallbackFailure(contextValue.error(), "on_finish");
    impl_->stopAfterCallbackError(contextValue.error());
    return contextValue.error();
  }
  const auto response = impl_->exchange(
      "callback",
      nlohmann::json{{"context", std::move(contextValue).value()},
                     {"hook", "on_finish"}},
      hookDeadline, core::ErrorCode::strategyHookTimeout, cancellation);
  if (!response.ok()) {
    impl_->captureCallbackFailure(response.error(), "on_finish", true);
    impl_->stopAfterCallbackError(response.error());
    return response.error();
  }
  if (response.value().messageType == "worker-error") {
    const auto captured =
        impl_->captureFailedHook(response.value(), "on_finish");
    if (!captured.ok()) {
      impl_->captureCallbackFailure(captured.error(), "on_finish", true);
      impl_->terminate(shutdownDeadline);
      return captured.error();
    }
    const auto error = decodeWorkerError(response.value(),
                                         core::ErrorCode::strategyRuntimeError);
    impl_->terminate(shutdownDeadline);
    return error;
  }
  static const std::set<std::string> callbackKeys{
      "commands", "logs", "omittedLogBytes", "omittedLogEntries"};
  if (response.value().messageType != "callback-result" ||
      !hasExactKeys(response.value().payload, callbackKeys)) {
    const auto error = makeStrategyError(
        core::ErrorCode::strategyProtocolViolation,
        "Python worker returned a malformed callback response.");
    impl_->captureCallbackFailure(error, "on_finish", true);
    impl_->terminate(shutdownDeadline);
    return error;
  }
  const auto captured =
      impl_->captureSuccessfulHook(response.value().payload, "on_finish");
  auto commands = decodeCommands(response.value().payload.at("commands"),
                                 context.symbol, false);
  if (!captured.ok() || !commands.ok()) {
    const auto &error = !captured.ok() ? captured.error() : commands.error();
    impl_->captureCallbackFailure(error, "on_finish", !captured.ok());
    impl_->terminate(shutdownDeadline);
    return error;
  }
  const auto shutdown = impl_->shutdown();
  if (!shutdown.ok()) {
    impl_->captureCallbackFailure(shutdown.error(), "on_finish", true);
    return shutdown.error();
  }
  return {};
}

void PythonWorkerStrategy::onCancel() noexcept {
  if (impl_ != nullptr) {
    impl_->shutdownNoexcept();
  }
}

strategy::StrategyDiagnosticBatch PythonWorkerStrategy::takeDiagnostics() {
  auto result = std::move(impl_->pendingDiagnostics);
  impl_->pendingDiagnostics = {};
  if (!impl_->stopped || impl_->streamDiagnosticsCollected) {
    return result;
  }
  impl_->streamDiagnosticsCollected = true;
  const auto streams = impl_->process.diagnostics();
  const auto appendStream = [&result](const std::string_view name,
                                      const CapturedDiagnosticStream &stream) {
    if (stream.bytesSeen == 0 && stream.omittedBytes == 0) {
      return;
    }
    const nlohmann::json fields{
        {"base64", encodeBase64(stream.retainedBytes)},
        {"bytesSeen", std::to_string(stream.bytesSeen)},
        {"omittedBytes", std::to_string(stream.omittedBytes)},
    };
    const auto canonical = serializeCanonicalJson(fields);
    if (!canonical.ok()) {
      result.complete = false;
      return;
    }
    result.records.push_back(strategy::StrategyDiagnostic{
        .kind = strategy::StrategyDiagnosticKind::log,
        .hook = "worker",
        .level = "warning",
        .errorCode = {},
        .message = std::string{name},
        .canonicalFieldsJson = canonical.value(),
        .sourceLine = std::nullopt,
        .sourceColumn = std::nullopt,
        .traceback = {},
        .omittedBytes = stream.omittedBytes,
        .omittedEntries = 0,
    });
    if (stream.omittedBytes > 0) {
      result.complete = false;
    }
  };
  appendStream("stdout", streams.standardOutput);
  appendStream("stderr", streams.standardError);
  return result;
}

PythonStrategyDiagnostics PythonWorkerStrategy::diagnostics() const {
  auto result = impl_->diagnosticData;
  result.streams = impl_->process.diagnostics();
  if (result.omittedLogEntries > 0 || result.omittedLogBytes > 0) {
    const nlohmann::json notice{
        {"fields",
         {{"omitted_bytes", result.omittedLogBytes},
          {"omitted_entries", result.omittedLogEntries}}},
        {"level", "warning"},
        {"message", "DiagnosticsTruncated"},
    };
    if (result.structuredLogs.size() < maximumStructuredLogEntries) {
      result.structuredLogs.push_back(notice);
    }
  }
  return result;
}

} // namespace bte::bindings::python
