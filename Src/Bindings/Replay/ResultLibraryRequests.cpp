#include "Bte/Bindings/ResultLibraryRequests.h"

#include "Bte/Core/Cancellation.h"

#if defined(BTE_ENABLE_TEST_HOOKS)
#include "ResultLibraryRequestsTestHooks.h"
#endif

#include <QFuture>
#include <QTimer>
#include <QtConcurrentRun>

#include <algorithm> // IWYU pragma: keep
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#if defined(BTE_ENABLE_TEST_HOOKS)
#include <thread>
#endif
#include <tuple> // IWYU pragma: keep
#include <utility>
#include <vector>

// QtConcurrent's instantiated argument types have this provider in libstdc++.
#if defined(__GLIBCXX__)
#include <iosfwd>
#include <type_traits>
#endif

namespace bte::bindings {

#if defined(BTE_ENABLE_TEST_HOOKS)
namespace {
std::atomic_bool blockNextDetails = false;
std::atomic_bool detailsWorkerStarted = false;
} // namespace

namespace testing {

void blockNextLibraryDetailsUntilCancelled() noexcept {
  detailsWorkerStarted.store(false);
  blockNextDetails.store(true);
}

bool libraryDetailsWorkerStarted() noexcept {
  return detailsWorkerStarted.load();
}

void clearResultLibraryRequestHooks() noexcept {
  blockNextDetails.store(false);
  detailsWorkerStarted.store(false);
}

} // namespace testing
#endif

struct ResultLibraryRequests::Impl final {
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

  template <typename ResultType, typename Work>
  std::uint64_t request(QObject *owner, Work work,
                        std::function<void(ResultType)> callback) {
    cancellation.requestCancellation();
    prune();
    cancellation = core::CancellationSource{};
    const auto token = cancellation.token();
    const auto currentGeneration = ++generation;
    futures.push_back(QtConcurrent::run(
        [this, owner, currentGeneration, token, work = std::move(work),
         callback = std::move(callback)]() mutable {
          auto result = std::make_shared<ResultType>(work(token));
          QTimer::singleShot(0, owner,
                             [this, currentGeneration,
                              callback = std::move(callback),
                              result]() mutable {
                               if (currentGeneration == generation.load()) {
                                 callback(std::move(*result));
                               }
                             });
        }));
    return currentGeneration;
  }
};

ResultLibraryRequests::ResultLibraryRequests(std::filesystem::path resultStore,
                                             std::filesystem::path dataStore,
                                             QObject *parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
  impl_->resultStore = std::move(resultStore);
  impl_->dataStore = std::move(dataStore);
}

ResultLibraryRequests::~ResultLibraryRequests() {
  cancel();
  for (auto &future : impl_->futures) {
    future.waitForFinished();
  }
}

std::uint64_t ResultLibraryRequests::requestCatalog(const bool trash,
                                                    CatalogCallback callback) {
  return impl_->request<CatalogResult>(
      this,
      [resultStore = impl_->resultStore, dataStore = impl_->dataStore,
       trash](const core::CancellationToken &token) -> CatalogResult {
        auto store = results::ResultStore::open(resultStore, dataStore, token);
        if (!store.ok()) {
          return store.error();
        }
        auto entries = trash ? store.value()->listTrash(token)
                             : store.value()->list(token);
        if (!entries.ok()) {
          return entries.error();
        }
        return ResultCatalogSnapshot{
            .entries = std::move(entries).value(),
            .notices = store.value()->recoveryNotices(),
        };
      },
      std::move(callback));
}

std::uint64_t ResultLibraryRequests::requestDetails(std::string resultId,
                                                    DetailsCallback callback) {
  return impl_->request<DetailsResult>(
      this,
      [resultStore = impl_->resultStore, dataStore = impl_->dataStore,
       resultId = std::move(resultId)](
          const core::CancellationToken &token) -> DetailsResult {
#if defined(BTE_ENABLE_TEST_HOOKS)
        if (blockNextDetails.exchange(false)) {
          detailsWorkerStarted.store(true);
          while (!token.isCancellationRequested()) {
            std::this_thread::yield();
          }
        }
#endif
        auto store = results::ResultStore::open(resultStore, dataStore, token);
        if (!store.ok()) {
          return store.error();
        }
        return store.value()->openResult(resultId, token);
      },
      std::move(callback));
}

std::uint64_t
ResultLibraryRequests::requestMoveToTrash(std::string resultId,
                                          ActionCallback callback) {
  return impl_->request<ActionResult>(
      this,
      [resultStore = impl_->resultStore, dataStore = impl_->dataStore,
       resultId = std::move(resultId)](
          const core::CancellationToken &token) -> ActionResult {
        auto store = results::ResultStore::open(resultStore, dataStore, token);
        if (!store.ok()) {
          return store.error();
        }
        if (token.isCancellationRequested()) {
          return core::makeError(core::ErrorCode::cancelled,
                                 "Result removal was cancelled");
        }
        return store.value()->moveToTrash(resultId);
      },
      std::move(callback));
}

std::uint64_t ResultLibraryRequests::requestRestore(std::string resultId,
                                                    ActionCallback callback) {
  return impl_->request<ActionResult>(
      this,
      [resultStore = impl_->resultStore, dataStore = impl_->dataStore,
       resultId = std::move(resultId)](
          const core::CancellationToken &token) -> ActionResult {
        auto store = results::ResultStore::open(resultStore, dataStore, token);
        if (!store.ok()) {
          return store.error();
        }
        if (token.isCancellationRequested()) {
          return core::makeError(core::ErrorCode::cancelled,
                                 "Result restore was cancelled");
        }
        return store.value()->restore(resultId);
      },
      std::move(callback));
}

std::uint64_t ResultLibraryRequests::requestRename(std::string resultId,
                                                   std::string displayName,
                                                   ActionCallback callback) {
  return impl_->request<ActionResult>(
      this,
      [resultStore = impl_->resultStore, dataStore = impl_->dataStore,
       resultId = std::move(resultId), displayName = std::move(displayName)](
          const core::CancellationToken &token) -> ActionResult {
        auto store = results::ResultStore::open(resultStore, dataStore, token);
        if (!store.ok()) {
          return store.error();
        }
        if (token.isCancellationRequested()) {
          return core::makeError(core::ErrorCode::cancelled,
                                 "Result rename was cancelled");
        }
        return store.value()->renameResult(resultId, displayName);
      },
      std::move(callback));
}

void ResultLibraryRequests::cancel() noexcept {
  impl_->cancellation.requestCancellation();
  ++impl_->generation;
}

} // namespace bte::bindings
