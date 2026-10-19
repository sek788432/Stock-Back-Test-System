#pragma once

#include "Bte/Bindings/ResultCatalogSnapshot.h"
#include "Bte/Core/Result.h"
#include "Bte/Results/ResultStore.h"

#include <QObject>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace bte::bindings {

/// Owns cancellable Result-library work and ignores superseded completions.
class ResultLibraryRequests final : public QObject {
public:
  using CatalogResult = core::Result<ResultCatalogSnapshot>;
  using DetailsResult = core::Result<results::OpenedResult>;
  using ActionResult = core::Result<void>;
  using CatalogCallback = std::function<void(CatalogResult)>;
  using DetailsCallback = std::function<void(DetailsResult)>;
  using ActionCallback = std::function<void(ActionResult)>;

  ResultLibraryRequests(std::filesystem::path resultStore,
                        std::filesystem::path dataStore,
                        QObject *parent = nullptr);
  ~ResultLibraryRequests() override;
  ResultLibraryRequests(const ResultLibraryRequests &) = delete;
  ResultLibraryRequests &operator=(const ResultLibraryRequests &) = delete;
  ResultLibraryRequests(ResultLibraryRequests &&) = delete;
  ResultLibraryRequests &operator=(ResultLibraryRequests &&) = delete;

  std::uint64_t requestCatalog(bool trash, CatalogCallback callback);
  std::uint64_t requestDetails(std::string resultId, DetailsCallback callback);
  std::uint64_t requestMoveToTrash(std::string resultId,
                                   ActionCallback callback);
  std::uint64_t requestRestore(std::string resultId, ActionCallback callback);
  std::uint64_t requestRename(std::string resultId, std::string displayName,
                              ActionCallback callback);
  void cancel() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace bte::bindings
