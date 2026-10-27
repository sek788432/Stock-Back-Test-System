#include "Bte/Core/CanonicalJson.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

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

TEST(CanonicalJsonTest, rejectsInvalidUtf8InObjectKeysAndStringValues) {
  const std::string invalidUtf8(1, static_cast<char>(0x80));
  auto invalidValue = nlohmann::json::object();
  invalidValue["text"] = invalidUtf8;
  EXPECT_FALSE(bte::core::serializeCanonicalJson(invalidValue).ok());

  auto invalidKey = nlohmann::json::object();
  invalidKey[invalidUtf8] = "value";
  EXPECT_FALSE(bte::core::serializeCanonicalJson(invalidKey).ok());
}

} // namespace
