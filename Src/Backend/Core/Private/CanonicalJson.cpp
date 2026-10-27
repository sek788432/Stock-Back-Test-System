#include "Bte/Core/CanonicalJson.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bte::core {
namespace {

constexpr std::uint64_t maximumSafeInteger = 9007199254740991ULL;
constexpr std::size_t maximumJsonNestingDepth = 128;

[[nodiscard]] Error invalidJson(std::string message) {
  return makeError(ErrorCode::invalidArgument, std::move(message));
}

class DuplicateKeyDetector final : public nlohmann::json_sax<nlohmann::json> {
public:
  bool null() override { return true; }
  bool boolean(const bool value) override {
    static_cast<void>(value);
    return true;
  }

  bool number_integer(number_integer_t value) override {
    if (value < -static_cast<number_integer_t>(maximumSafeInteger)) {
      failure_ = "JSON integer exceeds the exactly representable range.";
      return false;
    }
    return true;
  }

  bool number_unsigned(number_unsigned_t value) override {
    if (value > maximumSafeInteger) {
      failure_ = "JSON integer exceeds the exactly representable range.";
      return false;
    }
    return true;
  }

  bool number_float(number_float_t value, const string_t &) override {
    if (!std::isfinite(value)) {
      failure_ = "JSON number is not finite.";
      return false;
    }
    return true;
  }

  bool string(string_t &) override { return true; }
  bool binary(binary_t &) override { return false; }

  bool start_object(const std::size_t elementCount) override {
    static_cast<void>(elementCount);
    if (!enterContainer()) {
      return false;
    }
    objectKeys_.emplace_back();
    return true;
  }

  bool key(string_t &value) override {
    if (objectKeys_.empty() || !objectKeys_.back().insert(value).second) {
      failure_ = "JSON object contains a duplicate property.";
      return false;
    }
    return true;
  }

  bool end_object() override {
    if (objectKeys_.empty()) {
      return false;
    }
    objectKeys_.pop_back();
    --depth_;
    return true;
  }

  bool start_array(const std::size_t elementCount) override {
    static_cast<void>(elementCount);
    return enterContainer();
  }
  bool end_array() override {
    if (depth_ == 0) {
      return false;
    }
    --depth_;
    return true;
  }

  bool parse_error(std::size_t, const std::string &,
                   const nlohmann::json::exception &error) override {
    if (failure_.empty()) {
      failure_ = error.what();
    }
    return false;
  }

  [[nodiscard]] const std::string &errorMessage() const noexcept {
    return failure_;
  }

private:
  bool enterContainer() {
    if (depth_ >= maximumJsonNestingDepth) {
      failure_ = "JSON nesting exceeds the supported depth.";
      return false;
    }
    ++depth_;
    return true;
  }

  std::vector<std::unordered_set<std::string>> objectKeys_;
  std::string failure_;
  std::size_t depth_ = 0;
};

[[nodiscard]] bool appendUtf16Key(std::string_view utf8,
                                  std::vector<std::uint16_t> &result) {
  for (std::size_t index = 0; index < utf8.size();) {
    const auto first = static_cast<std::uint8_t>(utf8[index]);
    std::uint32_t codePoint = 0;
    std::size_t continuationCount = 0;
    if (first <= 0x7fU) {
      codePoint = first;
    } else if ((first & 0xe0U) == 0xc0U) {
      codePoint = first & 0x1fU;
      continuationCount = 1;
    } else if ((first & 0xf0U) == 0xe0U) {
      codePoint = first & 0x0fU;
      continuationCount = 2;
    } else if ((first & 0xf8U) == 0xf0U) {
      codePoint = first & 0x07U;
      continuationCount = 3;
    } else {
      return false;
    }
    if (index + continuationCount >= utf8.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
      const auto next = static_cast<std::uint8_t>(utf8[index + offset]);
      if ((next & 0xc0U) != 0x80U) {
        return false;
      }
      codePoint = (codePoint << 6U) | (next & 0x3fU);
    }
    if ((continuationCount == 1 && codePoint < 0x80U) ||
        (continuationCount == 2 && codePoint < 0x800U) ||
        (continuationCount == 3 && codePoint < 0x10000U) ||
        codePoint > 0x10ffffU ||
        (codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
      return false;
    }
    if (codePoint <= 0xffffU) {
      result.push_back(static_cast<std::uint16_t>(codePoint));
    } else {
      codePoint -= 0x10000U;
      result.push_back(
          static_cast<std::uint16_t>(0xd800U + (codePoint >> 10U)));
      result.push_back(
          static_cast<std::uint16_t>(0xdc00U + (codePoint & 0x3ffU)));
    }
    index += continuationCount + 1;
  }
  return true;
}

[[nodiscard]] Result<std::string> serializeString(std::string_view value) {
  try {
    return nlohmann::json(std::string{value})
        .dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
  } catch (const nlohmann::json::exception &) {
    return invalidJson("JSON string is not valid UTF-8.");
  }
}

[[nodiscard]] std::optional<std::string>
formatFixedNumber(const std::string &digits, const int decimalPosition) {
  if (decimalPosition > 0 && decimalPosition <= 21) {
    const auto integralDigits = static_cast<std::size_t>(decimalPosition);
    std::string result;
    if (digits.size() <= integralDigits) {
      result += digits;
      result.append(integralDigits - digits.size(), '0');
    } else {
      result.append(digits.data(), integralDigits);
      result.push_back('.');
      result.append(digits.data() + integralDigits,
                    digits.size() - integralDigits);
    }
    return result;
  }
  if (decimalPosition <= 0 && decimalPosition >= -5) {
    std::string result{"0."};
    result.append(static_cast<std::size_t>(-decimalPosition), '0');
    result += digits;
    return result;
  }
  return std::nullopt;
}

[[nodiscard]] std::string formatScientificNumber(const std::string &digits,
                                                 const int decimalPosition) {
  std::string result;
  result.push_back(digits.front());
  if (digits.size() > 1) {
    result.push_back('.');
    result.append(digits.begin() + 1, digits.end());
  }
  const auto scientificExponent = decimalPosition - 1;
  result.push_back('e');
  if (scientificExponent >= 0) {
    result.push_back('+');
  }
  result += std::to_string(scientificExponent);
  return result;
}

[[nodiscard]] Result<std::string> serializeNumber(double value) {
  if (!std::isfinite(value)) {
    return invalidJson("JSON numbers must be finite IEEE-754 values.");
  }
  if (value == 0.0) {
    return std::string{"0"};
  }

  std::array<char, 128> buffer{};
  const auto conversion =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::general);
  if (conversion.ec != std::errc{}) {
    return invalidJson("Could not serialize a JSON number.");
  }

  std::string_view shortest{
      buffer.data(), static_cast<std::size_t>(conversion.ptr - buffer.data())};
  bool negative = false;
  if (!shortest.empty() && shortest.front() == '-') {
    negative = true;
    shortest.remove_prefix(1);
  }

  const auto exponentOffset = shortest.find_first_of("eE");
  const auto mantissa = shortest.substr(0, exponentOffset);
  int explicitExponent = 0;
  if (exponentOffset != std::string_view::npos) {
    auto exponentText = shortest.substr(exponentOffset + 1);
    if (!exponentText.empty() && exponentText.front() == '+') {
      exponentText.remove_prefix(1);
    }
    const auto parsed = std::from_chars(
        exponentText.data(), exponentText.data() + exponentText.size(),
        explicitExponent);
    if (parsed.ec != std::errc{}) {
      return invalidJson("Could not normalize a JSON number exponent.");
    }
  }

  const auto decimalOffset = mantissa.find('.');
  const auto integerDigits =
      decimalOffset == std::string_view::npos ? mantissa.size() : decimalOffset;
  std::string digits;
  digits.reserve(mantissa.size());
  for (const auto character : mantissa) {
    if (character != '.') {
      digits.push_back(character);
    }
  }
  int decimalPosition = static_cast<int>(integerDigits) + explicitExponent;
  const auto leadingZeroCount = digits.find_first_not_of('0');
  if (leadingZeroCount == std::string::npos) {
    return std::string{"0"};
  }
  digits.erase(0, leadingZeroCount);
  decimalPosition -= static_cast<int>(leadingZeroCount);
  while (digits.size() > 1 && digits.back() == '0') {
    digits.pop_back();
  }

  auto formatted = formatFixedNumber(digits, decimalPosition);
  auto result = formatted.has_value()
                    ? std::move(formatted).value()
                    : formatScientificNumber(digits, decimalPosition);
  if (negative) {
    result.insert(result.begin(), '-');
  }
  return result;
}

[[nodiscard]] Result<void> appendCanonicalJson(const nlohmann::json &value,
                                               std::string &output,
                                               std::size_t depth);

[[nodiscard]] Result<void> appendArray(const nlohmann::json &value,
                                       std::string &output,
                                       const std::size_t depth) {
  output.push_back('[');
  bool first = true;
  for (const auto &element : value) {
    if (!first) {
      output.push_back(',');
    }
    first = false;
    const auto appended = appendCanonicalJson(element, output, depth + 1);
    if (!appended.ok()) {
      return appended.error();
    }
  }
  output.push_back(']');
  return {};
}

[[nodiscard]] Result<void> appendObject(const nlohmann::json &value,
                                        std::string &output,
                                        const std::size_t depth) {
  struct Property {
    std::string_view key;
    const nlohmann::json *value;
    std::vector<std::uint16_t> utf16;
  };
  std::vector<Property> properties;
  properties.reserve(value.size());
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
    Property property{iterator.key(), &iterator.value(), {}};
    if (!appendUtf16Key(property.key, property.utf16)) {
      return invalidJson("JSON object key is not valid UTF-8.");
    }
    properties.push_back(std::move(property));
  }
  std::sort(properties.begin(), properties.end(),
            [](const Property &left, const Property &right) {
              return std::lexicographical_compare(
                  left.utf16.begin(), left.utf16.end(), right.utf16.begin(),
                  right.utf16.end());
            });
  output.push_back('{');
  bool first = true;
  for (const auto &property : properties) {
    if (!first) {
      output.push_back(',');
    }
    first = false;
    const auto encodedKey = serializeString(property.key);
    if (!encodedKey.ok()) {
      return encodedKey.error();
    }
    output += encodedKey.value();
    output.push_back(':');
    const auto appended =
        appendCanonicalJson(*property.value, output, depth + 1);
    if (!appended.ok()) {
      return appended.error();
    }
  }
  output.push_back('}');
  return {};
}

[[nodiscard]] Result<void> appendCanonicalJson(const nlohmann::json &value,
                                               std::string &output,
                                               const std::size_t depth) {
  if (depth > maximumJsonNestingDepth) {
    return invalidJson("JSON nesting exceeds the supported depth.");
  }
  if (value.is_null()) {
    output += "null";
    return {};
  }
  if (value.is_boolean()) {
    output += value.get<bool>() ? "true" : "false";
    return {};
  }
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    if (number > maximumSafeInteger) {
      return invalidJson(
          "JSON integer exceeds the exactly representable range.");
    }
    output += std::to_string(number);
    return {};
  }
  if (value.is_number_integer()) {
    const auto number = value.get<std::int64_t>();
    if (number < -static_cast<std::int64_t>(maximumSafeInteger)) {
      return invalidJson(
          "JSON integer exceeds the exactly representable range.");
    }
    output += std::to_string(number);
    return {};
  }
  if (value.is_number_float()) {
    const auto number = serializeNumber(value.get<double>());
    if (!number.ok()) {
      return number.error();
    }
    output += number.value();
    return {};
  }
  if (value.is_string()) {
    const auto encoded = serializeString(value.get_ref<const std::string &>());
    if (!encoded.ok()) {
      return encoded.error();
    }
    output += encoded.value();
    return {};
  }
  if (value.is_array()) {
    return appendArray(value, output, depth);
  }
  if (value.is_object()) {
    return appendObject(value, output, depth);
  }
  return invalidJson("Unsupported JSON value type.");
}

} // namespace

Result<nlohmann::json> parseCanonicalJson(const std::string_view bytes) {
  DuplicateKeyDetector duplicateKeyDetector;
  if (!nlohmann::json::sax_parse(bytes.begin(), bytes.end(),
                                 &duplicateKeyDetector)) {
    return invalidJson("JSON payload is malformed or violates the JSON value "
                       "model: " +
                       duplicateKeyDetector.errorMessage());
  }
  try {
    return nlohmann::json::parse(bytes.begin(), bytes.end());
  } catch (const nlohmann::json::exception &) {
    return invalidJson("JSON payload is malformed.");
  }
}

Result<std::string> serializeCanonicalJson(const nlohmann::json &value) {
  std::string result;
  result.reserve(256);
  const auto appended = appendCanonicalJson(value, result, 0);
  if (!appended.ok()) {
    return appended.error();
  }
  return result;
}

} // namespace bte::core
