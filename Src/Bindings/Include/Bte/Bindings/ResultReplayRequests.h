#pragma once

#include "Bte/Bindings/ResultReplay.h"
#include "Bte/Core/Result.h"
#include "Bte/Results/ResultStore.h"

#include <QObject>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bte::bindings {

/// Owns cancellable Result catalog/open work and publishes only the newest
/// generation as an immutable queued value on this object's thread.
class ResultReplayRequests final : public QObject {
public:
  using CatalogResult = core::Result<std::vector<results::ResultSummary>>;
  using OpenResult = core::Result<std::shared_ptr<ResultReplay>>;
  using CatalogCallback = std::function<void(CatalogResult)>;
  using OpenCallback = std::function<void(OpenResult)>;

  explicit ResultReplayRequests(std::filesystem::path resultStore,
                                std::filesystem::path dataStore,
                                QObject *parent = nullptr);
  ~ResultReplayRequests() override;
  ResultReplayRequests(const ResultReplayRequests &) = delete;
  ResultReplayRequests &operator=(const ResultReplayRequests &) = delete;
  ResultReplayRequests(ResultReplayRequests &&) = delete;
  ResultReplayRequests &operator=(ResultReplayRequests &&) = delete;

  std::uint64_t requestCatalog(CatalogCallback callback);
  std::uint64_t requestOpen(std::string resultId,
                            ResultReplayTimeframe timeframe,
                            OpenCallback callback);
  void cancel() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace bte::bindings
