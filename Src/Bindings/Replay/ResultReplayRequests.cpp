#include "Bte/Bindings/ResultReplayRequests.h"

#include "Bte/Core/Cancellation.h"

#if defined(BTE_ENABLE_TEST_HOOKS)
#include "ResultReplayRequestsTestHooks.h"
#endif

#include <QFuture>
#include <QPointer>
#include <QTimer>
#include <QtConcurrentRun>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#if defined(BTE_ENABLE_TEST_HOOKS)
#include <thread>
#endif
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bte::bindings {
namespace {

#if defined(BTE_ENABLE_TEST_HOOKS)
std::atomic_bool blockNextOpen = false;
std::atomic_bool openWorkerStarted = false;
#endif

} // namespace

#if defined(BTE_ENABLE_TEST_HOOKS)
namespace testing {

void blockNextResultOpenUntilCancelled() noexcept {
  openWorkerStarted.store(false);
  blockNextOpen.store(true);
}

bool resultOpenWorkerStarted() noexcept { return openWorkerStarted.load(); }

void clearResultReplayRequestHooks() noexcept {
  blockNextOpen.store(false);
  openWorkerStarted.store(false);
}

} // namespace testing
#endif

struct ResultReplayRequests::Impl final {
  std::filesystem::path resultStore;
  std::filesystem::path dataStore;
  core::CancellationSource cancellation;
  std::atomic_uint64_t generation = 0;
  std::vector<QFuture<void>> futures;

  void prune() {
    std::erase_if(futures, [](const QFuture<void> &future) {
      return future.isFinished();
    });
  }
};

ResultReplayRequests::ResultReplayRequests(std::filesystem::path resultStore,
                                           std::filesystem::path dataStore,
                                           QObject *parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
  impl_->resultStore = std::move(resultStore);
  impl_->dataStore = std::move(dataStore);
}

ResultReplayRequests::~ResultReplayRequests() {
  cancel();
  for (auto &future : impl_->futures) {
    future.waitForFinished();
  }
}

std::uint64_t ResultReplayRequests::requestCatalog(CatalogCallback callback) {
  cancel();
  impl_->prune();
  impl_->cancellation = core::CancellationSource{};
  const auto token = impl_->cancellation.token();
  const auto generation = ++impl_->generation;
  const QPointer<ResultReplayRequests> owner{this};
  impl_->futures.push_back(QtConcurrent::run(
      [owner, generation, token, callback = std::move(callback),
       resultStore = impl_->resultStore,
       dataStore = impl_->dataStore]() mutable {
        auto result = std::make_shared<CatalogResult>(
            ResultReplay::list(resultStore, dataStore, token));
        if (owner == nullptr) {
          return;
        }
        QTimer::singleShot(0, owner,
                           [owner, generation, callback = std::move(callback),
                            result]() mutable {
                             if (owner != nullptr &&
                                 generation ==
                                     owner->impl_->generation.load()) {
                               callback(std::move(*result));
                             }
                           });
      }));
  return generation;
}

std::uint64_t
ResultReplayRequests::requestOpen(std::string resultId,
                                  const ResultReplayTimeframe timeframe,
                                  OpenCallback callback) {
  cancel();
  impl_->prune();
  impl_->cancellation = core::CancellationSource{};
  const auto token = impl_->cancellation.token();
  const auto generation = ++impl_->generation;
  const QPointer<ResultReplayRequests> owner{this};
  impl_->futures.push_back(QtConcurrent::run(
      [owner, generation, token, callback = std::move(callback),
       resultStore = impl_->resultStore, dataStore = impl_->dataStore,
       resultId = std::move(resultId), timeframe]() mutable {
#if defined(BTE_ENABLE_TEST_HOOKS)
        if (blockNextOpen.exchange(false)) {
          openWorkerStarted.store(true);
          while (!token.isCancellationRequested()) {
            std::this_thread::yield();
          }
        }
#endif
        auto opened = ResultReplay::open(resultStore, dataStore, resultId,
                                         timeframe, token);
        auto result = std::make_shared<OpenResult>(
            opened.ok() ? OpenResult{std::shared_ptr<ResultReplay>{
                              std::move(opened).value()}}
                        : OpenResult{opened.error()});
        if (owner == nullptr) {
          return;
        }
        QTimer::singleShot(0, owner,
                           [owner, generation, callback = std::move(callback),
                            result]() mutable {
                             if (owner != nullptr &&
                                 generation ==
                                     owner->impl_->generation.load()) {
                               callback(std::move(*result));
                             }
                           });
      }));
  return generation;
}

void ResultReplayRequests::cancel() noexcept {
  impl_->cancellation.requestCancellation();
  ++impl_->generation;
}

} // namespace bte::bindings
