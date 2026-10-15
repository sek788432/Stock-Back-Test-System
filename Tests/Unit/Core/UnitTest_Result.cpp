#include "Bte/Core/Digest.h"
#include "Bte/Core/Result.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST(ResultTest, valueResult_reportsOkAndReturnsValue) {
  const bte::core::Result<std::string> result{std::string{"ok"}};

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), "ok");
}

TEST(ResultTest, errorResult_reportsErrorAndReturnsStructuredCode) {
  const bte::core::Result<int> result{
      bte::core::makeError(bte::core::ErrorCode::notFound, "missing fixture")};

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::notFound);
  EXPECT_EQ(result.error().message, "missing fixture");
  EXPECT_TRUE(static_cast<bool>(result.error()));
}

TEST(ResultTest, valueAccessOnErrorThrowsForConstAndMovedResults) {
  const bte::core::Result<int> constResult{
      bte::core::makeError(bte::core::ErrorCode::notFound, "missing")};
  auto movedResult = bte::core::Result<std::string>{
      bte::core::makeError(bte::core::ErrorCode::notFound, "missing")};

  EXPECT_THROW(static_cast<void>(constResult.value()),
               std::bad_optional_access);
  EXPECT_THROW(static_cast<void>(std::move(movedResult).value()),
               std::bad_optional_access);
}

TEST(ResultTest, voidResultDistinguishesSuccessFromStructuredFailure) {
  const bte::core::Result<void> success;
  const bte::core::Result<void> failure{
      bte::core::makeError(bte::core::ErrorCode::permissionDenied, "denied")};

  EXPECT_TRUE(success.ok());
  EXPECT_FALSE(failure.ok());
  EXPECT_EQ(failure.error().code, bte::core::ErrorCode::permissionDenied);
  EXPECT_EQ(failure.error().message, "denied");
}

TEST(DigestTest, sha256MatchesPublishedEmptyAndAbcVectors) {
  EXPECT_EQ(bte::core::sha256(""), "e3b0c44298fc1c149afbf4c8996fb924"
                                   "27ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(bte::core::sha256("abc"), "ba7816bf8f01cfea414140de5dae2223"
                                      "b00361a396177a9cb410ff61f20015ad");
}

TEST(DigestTest, sha256MatchesIndependentBinaryPaddingBoundaryVectors) {
  struct Vector final {
    std::size_t length;
    std::string_view digest;
  };
  // Independent Python hashlib SHA-256 values for bytes 0, 1, ... length-1.
  // Cover both sides of the length-field boundary in the first two blocks.
  constexpr std::array vectors{
      Vector{
          55,
          "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
      Vector{
          56,
          "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
      Vector{
          63,
          "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
      Vector{
          64,
          "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
      Vector{
          65,
          "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
      Vector{
          119,
          "da18797ed7c3a777f0847f429724a2d8cd5138e6ed2895c3fa1a6d39d18f7ec6"},
      Vector{
          120,
          "f52b23db1fbb6ded89ef42a23ce0c8922c45f25c50b568a93bf1c075420bbb7c"},
      Vector{
          127,
          "92ca0fa6651ee2f97b884b7246a562fa71250fedefe5ebf270d31c546bfea976"},
      Vector{
          128,
          "471fb943aa23c511f6f72f8d1652d9c880cfa392ad80503120547703e56a2be5"},
      Vector{
          129,
          "5099c6a56203f9687f7d33f4bfdf576d31dc91f6b695ecea38b2770c87631135"},
      Vector{
          256,
          "40aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880"}};
  for (const auto &[length, digest] : vectors) {
    SCOPED_TRACE(length);
    std::vector<std::byte> bytes;
    bytes.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
      bytes.push_back(static_cast<std::byte>(index));
    }
    EXPECT_EQ(bte::core::sha256(std::span<const std::byte>{bytes}), digest);
  }
}

TEST(DigestTest, sha256MatchesPublishedMillionAVector) {
  EXPECT_EQ(bte::core::sha256(std::string(1'000'000, 'a')),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(DigestTest, sha256HonorsBorrowedSubspansWithoutHashingSurroundingBytes) {
  std::vector<std::byte> storage(67, std::byte{0xff});
  for (std::size_t index = 1; index <= 65; ++index) {
    storage[index] = static_cast<std::byte>(index - 1);
  }
  const auto view = std::span<const std::byte>{storage};
  EXPECT_EQ(bte::core::sha256(view.subspan(1, 65)),
            "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781");
  EXPECT_EQ(bte::core::sha256(view.subspan(1, 0)),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  const std::string text{"xabcx"};
  EXPECT_EQ(bte::core::sha256(std::string_view{text}.substr(1, 3)),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
