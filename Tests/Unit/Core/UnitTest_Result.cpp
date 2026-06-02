#include "Bte/Core/Result.h"

#include <gtest/gtest.h>

#include <string>

TEST(ResultTest, valueResult_reportsOkAndReturnsValue) {
    const bte::core::Result<std::string> result{std::string{"ok"}};

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), "ok");
}

TEST(ResultTest, errorResult_reportsErrorAndReturnsStructuredCode) {
    const bte::core::Result<int> result{bte::core::makeError(bte::core::ErrorCode::notFound, "missing fixture")};

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, bte::core::ErrorCode::notFound);
    EXPECT_EQ(result.error().message, "missing fixture");
    EXPECT_TRUE(static_cast<bool>(result.error()));
}
