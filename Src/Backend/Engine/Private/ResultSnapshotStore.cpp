#include "Bte/Engine/ResultSnapshotStore.h"

#include "Bte/Core/Result.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace bte::engine {
namespace {

std::string escapeJson(const std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += ch;
      break;
    }
  }
  return escaped;
}

std::string jsonString(const std::string_view value) {
  return "\"" + escapeJson(value) + "\"";
}

std::string snapshotFileStem(const ReplayResultSnapshot &snapshot) {
  std::string stem = snapshot.generatedAtIso8601 + "-" + snapshot.symbol + "-" +
                     snapshot.strategyArtifactHash;
  std::ranges::replace_if(
      stem,
      [](const char ch) {
        return !std::isalnum(static_cast<unsigned char>(ch));
      },
      '-');
  return stem.empty() ? "replay-result" : stem;
}

std::string toJson(const ReplayResultSnapshot &snapshot) {
  std::ostringstream output;
  output << std::setprecision(15);
  output << "{\n";
  output << "\"schemaVersion\":1,\n";
  output << "\"symbol\":" << jsonString(snapshot.symbol) << ",\n";
  output << "\"schemaName\":" << jsonString(snapshot.schemaName) << ",\n";
  output << "\"startIso8601\":" << jsonString(snapshot.startIso8601) << ",\n";
  output << "\"endIso8601\":" << jsonString(snapshot.endIso8601) << ",\n";
  output << "\"initialCapital\":" << snapshot.initialCapital << ",\n";
  output << "\"strategyName\":" << jsonString(snapshot.strategyName) << ",\n";
  output << "\"strategyArtifactHash\":"
         << jsonString(snapshot.strategyArtifactHash) << ",\n";
  output << "\"engineConfigHash\":" << jsonString(snapshot.engineConfigHash)
         << ",\n";
  output << "\"dataSnapshot\":" << jsonString(snapshot.dataSnapshot) << ",\n";
  output << "\"generatedAtIso8601\":" << jsonString(snapshot.generatedAtIso8601)
         << ",\n";
  output << "\"metrics\":{";
  output << "\"totalReturn\":" << snapshot.metrics.totalReturn << ",";
  output << "\"maxDrawdown\":" << snapshot.metrics.maxDrawdown << ",";
  output << "\"winRate\":" << snapshot.metrics.winRate << ",";
  output << "\"tradeCount\":" << snapshot.metrics.tradeCount << ",";
  output << "\"finalEquity\":" << snapshot.metrics.finalEquity;
  output << "},\n";
  output << "\"equityCurve\":[";
  for (std::size_t index = 0; index < snapshot.equityCurve.size(); ++index) {
    const auto &point = snapshot.equityCurve[index];
    if (index > 0) {
      output << ",";
    }
    output << "{\"barIndex\":" << point.barIndex
           << ",\"equity\":" << point.equity << "}";
  }
  output << "],\n";
  output << "\"tradeLog\":[";
  for (std::size_t index = 0; index < snapshot.tradeLog.size(); ++index) {
    const auto &trade = snapshot.tradeLog[index];
    if (index > 0) {
      output << ",";
    }
    output << "{\"barIndex\":" << trade.barIndex
           << ",\"side\":" << jsonString(trade.side)
           << ",\"price\":" << trade.price << ",\"quantity\":" << trade.quantity
           << ",\"pnl\":" << trade.pnl << "}";
  }
  output << "]\n";
  output << "}\n";
  return output.str();
}

core::Result<std::string> readAll(const std::filesystem::path &path) {
  std::ifstream input{path};
  if (!input) {
    return core::makeError(core::ErrorCode::notFound,
                           "Result snapshot file not found: " + path.string());
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::string extractString(const std::string &json, const std::string &key) {
  const auto pattern = "\"" + key + "\":\"";
  const auto start = json.find(pattern);
  if (start == std::string::npos) {
    return {};
  }
  const auto valueStart = start + pattern.size();
  const auto valueEnd = json.find('"', valueStart);
  if (valueEnd == std::string::npos) {
    return {};
  }
  return json.substr(valueStart, valueEnd - valueStart);
}

double extractDouble(const std::string &json, const std::string &key) {
  const auto pattern = "\"" + key + "\":";
  const auto start = json.find(pattern);
  if (start == std::string::npos) {
    return 0.0;
  }
  const auto valueStart = start + pattern.size();
  const auto valueEnd = json.find_first_of(",}\n", valueStart);
  return std::stod(json.substr(valueStart, valueEnd - valueStart));
}

int extractInt(const std::string &json, const std::string &key) {
  return static_cast<int>(extractDouble(json, key));
}

std::string compactNumber(const double value) {
  std::ostringstream output;
  output << std::setprecision(15) << value;
  return output.str();
}

std::vector<ComparisonMetricValue>
metricValues(const std::vector<ReplayResultSummary> &summaries,
             double ReplayResultSummary::*member) {
  std::vector<ComparisonMetricValue> values;
  values.reserve(summaries.size());
  for (const auto &summary : summaries) {
    values.push_back(ComparisonMetricValue{
        .label = summary.strategyName,
        .value = compactNumber(summary.*member),
    });
  }
  return values;
}

std::vector<ComparisonMetricValue>
metricValues(const std::vector<ReplayResultSummary> &summaries,
             int ReplayResultSummary::*member) {
  std::vector<ComparisonMetricValue> values;
  values.reserve(summaries.size());
  for (const auto &summary : summaries) {
    values.push_back(ComparisonMetricValue{
        .label = summary.strategyName,
        .value = std::to_string(summary.*member),
    });
  }
  return values;
}

} // namespace

core::Result<std::filesystem::path>
saveReplayResultSnapshot(const std::filesystem::path &directory,
                         const ReplayResultSnapshot &snapshot) {
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Cannot create result snapshot directory: " +
                               directory.string());
  }

  const auto path = directory / (snapshotFileStem(snapshot) + ".json");
  std::ofstream output{path, std::ios::trunc};
  if (!output) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Cannot write result snapshot: " + path.string());
  }

  output << toJson(snapshot);
  return path;
}

core::Result<std::vector<ReplayResultSummary>>
listReplayResultSnapshots(const std::filesystem::path &directory) {
  std::error_code ec;
  if (!std::filesystem::exists(directory, ec)) {
    return std::vector<ReplayResultSummary>{};
  }

  std::vector<ReplayResultSummary> summaries;
  for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
    if (ec) {
      return core::makeError(core::ErrorCode::internal,
                             "Cannot enumerate result snapshots: " +
                                 directory.string());
    }
    if (!entry.is_regular_file() || entry.path().extension() != ".json") {
      continue;
    }

    auto contents = readAll(entry.path());
    if (!contents.ok()) {
      return contents.error();
    }
    summaries.push_back(ReplayResultSummary{
        .path = entry.path(),
        .symbol = extractString(contents.value(), "symbol"),
        .schemaName = extractString(contents.value(), "schemaName"),
        .strategyName = extractString(contents.value(), "strategyName"),
        .totalReturn = extractDouble(contents.value(), "totalReturn"),
        .maxDrawdown = extractDouble(contents.value(), "maxDrawdown"),
        .winRate = extractDouble(contents.value(), "winRate"),
        .tradeCount = extractInt(contents.value(), "tradeCount"),
        .finalEquity = extractDouble(contents.value(), "finalEquity"),
    });
  }

  std::ranges::sort(summaries, {}, &ReplayResultSummary::path);
  return summaries;
}

std::vector<ComparisonMetricRow> compareReplayResultSummaries(
    const std::vector<ReplayResultSummary> &summaries) {
  return {
      {.metricName = "Final Equity",
       .values = metricValues(summaries, &ReplayResultSummary::finalEquity)},
      {.metricName = "Total Return",
       .values = metricValues(summaries, &ReplayResultSummary::totalReturn)},
      {.metricName = "Max Drawdown",
       .values = metricValues(summaries, &ReplayResultSummary::maxDrawdown)},
      {.metricName = "Win Rate",
       .values = metricValues(summaries, &ReplayResultSummary::winRate)},
      {.metricName = "Trade Count",
       .values = metricValues(summaries, &ReplayResultSummary::tradeCount)},
  };
}

} // namespace bte::engine
