#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace bte::core {

class CancellationToken final {
public:
  CancellationToken() = default;

  [[nodiscard]] bool isCancellationRequested() const noexcept {
    return flag_ != nullptr && flag_->load();
  }

private:
  friend class CancellationSource;

  explicit CancellationToken(std::shared_ptr<const std::atomic_bool> flag)
      : flag_(std::move(flag)) {}

  std::shared_ptr<const std::atomic_bool> flag_;
};

class CancellationSource final {
public:
  CancellationSource() : flag_(std::make_shared<std::atomic_bool>(false)) {}

  [[nodiscard]] CancellationToken token() const {
    return CancellationToken{flag_};
  }

  void requestCancellation() noexcept { flag_->store(true); }

private:
  std::shared_ptr<std::atomic_bool> flag_;
};

} // namespace bte::core
