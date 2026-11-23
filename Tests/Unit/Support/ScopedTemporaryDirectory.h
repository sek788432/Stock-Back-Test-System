#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace bte::test_support {

class ScopedTemporaryDirectory final {
public:
  explicit ScopedTemporaryDirectory(
      const std::string_view prefix = "bte-test") {
    static std::atomic_uint64_t nextId{0};
    std::random_device random;
    const auto base = std::filesystem::temp_directory_path();
    for (auto attempt = 0; attempt < 32; ++attempt) {
      const auto stamp =
          std::chrono::steady_clock::now().time_since_epoch().count();
      path_ = base /
              (std::string{prefix} + "-" + std::to_string(stamp) + "-" +
               std::to_string(random()) + "-" +
               std::to_string(nextId.fetch_add(1, std::memory_order_relaxed)));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
      if (error && error != std::errc::file_exists) {
        throw std::filesystem::filesystem_error{
            "Unable to create unique test directory", path_, error};
      }
    }
    throw std::runtime_error{"Unable to allocate a unique test directory"};
  }

  ~ScopedTemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  ScopedTemporaryDirectory(const ScopedTemporaryDirectory &) = delete;
  ScopedTemporaryDirectory &
  operator=(const ScopedTemporaryDirectory &) = delete;
  ScopedTemporaryDirectory(ScopedTemporaryDirectory &&) = delete;
  ScopedTemporaryDirectory &operator=(ScopedTemporaryDirectory &&) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

} // namespace bte::test_support
