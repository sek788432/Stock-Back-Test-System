#pragma once

#include <QCoreApplication>

#include <chrono>
#include <concepts>
#include <thread>

namespace bte::test {

template <typename Predicate>
  requires std::predicate<Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate,
                             const std::chrono::milliseconds timeout =
                                 std::chrono::milliseconds{5'000}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return true;
}

} // namespace bte::test
