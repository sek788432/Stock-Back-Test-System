#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"
#include "Bte/Results/ResultStore.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bte::bindings {

enum class ResultReplayTimeframe : std::uint8_t { hourly, dailyUtc };

struct ResultReplayFill {
  core::Timestamp timestamp;
  bool isBuy = false;
  std::int64_t quantityShares = 0;
  double price = 0.0;
  double amount = 0.0;

  bool operator==(const ResultReplayFill &) const = default;
};

struct ResultReplayPortfolio {
  double cash = 0.0;
  std::int64_t positionShares = 0;
  double marketValue = 0.0;
  double equity = 0.0;
  double pnl = 0.0;

  bool operator==(const ResultReplayPortfolio &) const = default;
};

struct ResultReplayFrame {
  core::Bar candle;
  std::vector<ResultReplayFill> fills;
  ResultReplayPortfolio portfolio;
  bool partialUtcDay = false;

  bool operator==(const ResultReplayFrame &) const = default;
};

class ResultReplay final {
private:
  struct ConstructionKey final {};

public:
  explicit ResultReplay(ConstructionKey, std::string resultId,
                        std::vector<ResultReplayFrame> frames,
                        std::string terminalReason);

  [[nodiscard]] static core::Result<std::unique_ptr<ResultReplay>>
  open(const std::filesystem::path &resultStore,
       const std::filesystem::path &dataStore, const std::string &resultId,
       ResultReplayTimeframe timeframe,
       const core::CancellationToken &cancellation = {});

  [[nodiscard]] static core::Result<std::vector<results::ResultSummary>>
  list(const std::filesystem::path &resultStore,
       const std::filesystem::path &dataStore,
       const core::CancellationToken &cancellation = {});

  [[nodiscard]] const std::string &resultId() const noexcept;
  [[nodiscard]] const ResultReplayFrame *current() const noexcept;
  [[nodiscard]] bool stepForward() noexcept;
  [[nodiscard]] bool stepBack() noexcept;
  [[nodiscard]] bool seek(std::size_t index) noexcept;
  [[nodiscard]] std::size_t currentIndex() const noexcept;
  [[nodiscard]] std::size_t totalFrames() const noexcept;
  [[nodiscard]] int progressPercent() const noexcept;
  [[nodiscard]] std::vector<ResultReplayFrame> visibleWindow() const;
  [[nodiscard]] const std::string &terminalReason() const noexcept;

private:
  std::string resultId_;
  std::vector<ResultReplayFrame> frames_;
  std::string terminalReason_;
  std::size_t currentIndex_ = 0;
};

} // namespace bte::bindings
