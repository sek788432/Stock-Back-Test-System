#include "Bte/Core/Cancellation.h"

#include <gtest/gtest.h>

TEST(CancellationTest, defaultTokenIsNotCancelled) {
  const bte::core::CancellationToken token;

  EXPECT_FALSE(token.isCancellationRequested());
}

TEST(CancellationTest, copiedTokensObserveSourceCancellation) {
  bte::core::CancellationSource source;
  const auto first = source.token();
  const auto copy = first;

  EXPECT_FALSE(first.isCancellationRequested());
  EXPECT_FALSE(copy.isCancellationRequested());

  source.requestCancellation();

  EXPECT_TRUE(first.isCancellationRequested());
  EXPECT_TRUE(copy.isCancellationRequested());
}
