#pragma once

#include "Bte/Core/Result.h"

#include <filesystem>
#include <string>
#include <vector>

namespace bte::engine {

struct EquityCurvePoint {
  int barIndex = 0;
  double equity = 0.0;
};

struct TradeLogRow {
  int barIndex = 0;
  std::string side;
  double price = 0.0;
  double quantity = 0.0;
  double pnl = 0.0;
};

struct ResultMetrics {
  double totalReturn = 0.0;
  double maxDrawdown = 0.0;
  double winRate = 0.0;
  int tradeCount = 0;
  double finalEquity = 0.0;
};

struct ReplayResultSnapshot {
  std::string symbol;
  std::string schemaName;
  std::string startIso8601;
  std::string endIso8601;
  double initialCapital = 0.0;
  std::string strategyName;
  std::string strategyArtifactHash;
  std::string engineConfigHash;
  std::string dataSnapshot;
  std::string generatedAtIso8601;
  ResultMetrics metrics;
  std::vector<EquityCurvePoint> equityCurve;
  std::vector<TradeLogRow> tradeLog;
};

struct ReplayResultSummary {
  std::filesystem::path path;
  std::string symbol;
  std::string schemaName;
  std::string strategyName;
  double totalReturn = 0.0;
  double maxDrawdown = 0.0;
  double winRate = 0.0;
  int tradeCount = 0;
  double finalEquity = 0.0;
};

struct ComparisonMetricValue {
  std::string label;
  std::string value;
};

struct ComparisonMetricRow {
  std::string metricName;
  std::vector<ComparisonMetricValue> values;
};

[[nodiscard]] core::Result<std::filesystem::path>
saveReplayResultSnapshot(const std::filesystem::path &directory,
                         const ReplayResultSnapshot &snapshot);

[[nodiscard]] core::Result<std::vector<ReplayResultSummary>>
listReplayResultSnapshots(const std::filesystem::path &directory);

[[nodiscard]] std::vector<ComparisonMetricRow>
compareReplayResultSummaries(const std::vector<ReplayResultSummary> &summaries);

} // namespace bte::engine
