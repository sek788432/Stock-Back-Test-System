#include "Bte/Core/CanonicalJson.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

TEST(CanonicalJsonTest, serializesRfc8785NumberAndStringVectors) {
  const auto value = bte::core::parseCanonicalJson(
      R"json({"number":333333333.33333329,"small":0.000001,"tiny":1e-7,"large":1e30,"whole":1.0,"negativeZero":-0.0,"text":"€$\u000f\u000aA'B\u0022\\\u0022/"})json");
  ASSERT_TRUE(value.ok()) << value.error().message;

  const auto bytes = bte::core::serializeCanonicalJson(value.value());

  ASSERT_TRUE(bytes.ok()) << bytes.error().message;
  EXPECT_EQ(
      bytes.value(),
      R"json({"large":1e+30,"negativeZero":0,"number":333333333.3333333,"small":0.000001,"text":"€$\u000f\nA'B\"\\\"/","tiny":1e-7,"whole":1})json");
}

TEST(CanonicalJsonTest, normalizesSignedFixedAndScientificBinary64Values) {
  const nlohmann::json value{
      {"negativeFixed", -12.5},
      {"negativeScientific", -1.25e-7},
      {"positiveScientific", 1.25e30},
      {"smallFixed", 0.0000012},
  };

  const auto bytes = bte::core::serializeCanonicalJson(value);

  ASSERT_TRUE(bytes.ok()) << bytes.error().message;
  EXPECT_EQ(
      bytes.value(),
      R"json({"negativeFixed":-12.5,"negativeScientific":-1.25e-7,"positiveScientific":1.25e+30,"smallFixed":0.0000012})json");
}

TEST(CanonicalJsonTest, sortsObjectKeysByUtf16CodeUnits) {
  const auto value = bte::core::parseCanonicalJson(
      R"json({"€":"Euro","\r":"Carriage","﮳":"Hebrew","1":"One","😀":"Emoji","\u0080":"Control","ö":"Latin"})json");
  ASSERT_TRUE(value.ok()) << value.error().message;

  const auto bytes = bte::core::serializeCanonicalJson(value.value());

  ASSERT_TRUE(bytes.ok()) << bytes.error().message;
  EXPECT_EQ(
      bytes.value(),
      R"json({"\r":"Carriage","1":"One","":"Control","ö":"Latin","€":"Euro","😀":"Emoji","﮳":"Hebrew"})json");
}

TEST(CanonicalJsonTest,
     rejectsDuplicateKeysOutOfModelNumbersAndExcessiveNesting) {
  EXPECT_FALSE(
      bte::core::parseCanonicalJson(R"json({"key":1,"key":2})json").ok());
  const auto exactIntegerBoundaries = bte::core::parseCanonicalJson(
      R"json({"max":9007199254740991,"min":-9007199254740991})json");
  ASSERT_TRUE(exactIntegerBoundaries.ok())
      << exactIntegerBoundaries.error().message;
  const auto exactIntegerBytes =
      bte::core::serializeCanonicalJson(exactIntegerBoundaries.value());
  ASSERT_TRUE(exactIntegerBytes.ok()) << exactIntegerBytes.error().message;
  EXPECT_EQ(exactIntegerBytes.value(),
            R"json({"max":9007199254740991,"min":-9007199254740991})json");
  EXPECT_FALSE(
      bte::core::parseCanonicalJson(R"json({"value":9007199254740992})json")
          .ok());
  const std::string nestedBoundary(128, '[');
  const auto atNestingLimit = bte::core::parseCanonicalJson(
      nestedBoundary + "0" + std::string(128, ']'));
  ASSERT_TRUE(atNestingLimit.ok()) << atNestingLimit.error().message;
  const std::string nested(129, '[');
  EXPECT_FALSE(
      bte::core::parseCanonicalJson(nested + "0" + std::string(129, ']')).ok());

  auto nonFinite = nlohmann::json::object();
  nonFinite["value"] = std::numeric_limits<double>::infinity();
  const auto serialized = bte::core::serializeCanonicalJson(nonFinite);
  EXPECT_FALSE(serialized.ok());
  EXPECT_EQ(serialized.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(CanonicalJsonTest, enforcesNestingLimitForObjects) {
  const auto makeNestedObject = [](const std::size_t depth) {
    std::string json;
    json.reserve(depth * 8U + 1U);
    for (std::size_t index = 0; index < depth; ++index) {
      json += R"json({"nested":)json";
    }
    json += "0";
    json.append(depth, '}');
    return json;
  };

  const auto atLimit = bte::core::parseCanonicalJson(makeNestedObject(128));
  ASSERT_TRUE(atLimit.ok()) << atLimit.error().message;
  const auto aboveLimit = bte::core::parseCanonicalJson(makeNestedObject(129));
  ASSERT_FALSE(aboveLimit.ok());
  EXPECT_EQ(aboveLimit.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_NE(aboveLimit.error().message.find("nesting"), std::string::npos);
}

TEST(CanonicalJsonTest, rejectsInvalidUtf8InObjectKeysAndStringValues) {
  const std::string invalidUtf8(1, static_cast<char>(0x80));
  auto invalidValue = nlohmann::json::object();
  invalidValue["text"] = invalidUtf8;
  EXPECT_FALSE(bte::core::serializeCanonicalJson(invalidValue).ok());

  auto invalidKey = nlohmann::json::object();
  invalidKey[invalidUtf8] = "value";
  EXPECT_FALSE(bte::core::serializeCanonicalJson(invalidKey).ok());
}

TEST(CanonicalJsonTest, rejectsInvalidUtf8InParsedKeysAndStringValues) {
  const std::string invalidUtf8(1, static_cast<char>(0x80));
  const std::string invalidValue = "\"" + invalidUtf8 + "\"";
  const std::string invalidKey = "{\"" + invalidUtf8 + "\":1}";

  for (const auto &input : {invalidValue, invalidKey}) {
    SCOPED_TRACE(input.size());
    const auto parsed = bte::core::parseCanonicalJson(input);

    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.error().code, bte::core::ErrorCode::invalidArgument);
    EXPECT_FALSE(parsed.error().message.empty());
  }
}

TEST(CanonicalJsonTest, acceptsJsonScalarsAndRejectsMalformedOrUnsafeNumbers) {
  for (const auto input :
       {"null", "true", "false", "0", "-1", "1.25", "\"text\"", "[]", "{}"}) {
    const auto parsed = bte::core::parseCanonicalJson(input);
    ASSERT_TRUE(parsed.ok()) << input << ": " << parsed.error().message;
  }

  for (const auto input :
       {"", "{", "[1,]", "true false", R"({"outer":{"key":1,"key":2}})"}) {
    EXPECT_FALSE(bte::core::parseCanonicalJson(input).ok()) << input;
  }
  EXPECT_FALSE(bte::core::parseCanonicalJson("-9007199254740992").ok());
  EXPECT_FALSE(bte::core::parseCanonicalJson("1e400").ok());
}

TEST(CanonicalJsonTest, rejectsUnsafeIntegerBinaryAndExcessivelyNestedValues) {
  const auto oversizedUnsigned =
      nlohmann::json::number_unsigned_t{9007199254740992ULL};
  EXPECT_FALSE(bte::core::serializeCanonicalJson(oversizedUnsigned).ok());
  EXPECT_FALSE(bte::core::serializeCanonicalJson(
                   std::numeric_limits<std::int64_t>::min())
                   .ok());
  EXPECT_FALSE(
      bte::core::serializeCanonicalJson(nlohmann::json::binary({0x01U})).ok());

  auto nested = nlohmann::json::array();
  for (int index = 0; index < 130; ++index) {
    nested = nlohmann::json::array({std::move(nested)});
  }
  EXPECT_FALSE(bte::core::serializeCanonicalJson(nested).ok());
}

TEST(CanonicalJsonTest, rejectsMalformedUtf8SequencesInObjectKeys) {
  const std::vector<std::string> invalidKeys{
      std::string{static_cast<char>(0x80)},
      std::string{static_cast<char>(0xc2)},
      std::string{static_cast<char>(0xc2), 'A'},
      std::string{static_cast<char>(0xe0), static_cast<char>(0x80),
                  static_cast<char>(0x80)},
      std::string{static_cast<char>(0xed), static_cast<char>(0xa0),
                  static_cast<char>(0x80)},
      std::string{static_cast<char>(0xf4), static_cast<char>(0x90),
                  static_cast<char>(0x80), static_cast<char>(0x80)},
  };
  for (const auto &key : invalidKeys) {
    auto value = nlohmann::json::object();
    value[key] = 1;
    EXPECT_FALSE(bte::core::serializeCanonicalJson(value).ok());
  }
}

} // namespace
