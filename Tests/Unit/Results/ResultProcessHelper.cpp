#include "Bte/Core/Result.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Results/ResultStore.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bte::core::Result<bte::results::RunDescriptor>
createFixture(const std::filesystem::path &root) {
  std::filesystem::create_directories(root / "Source");
  std::ofstream source{root / "Source" / "SYN.csv"};
  source << "symbol,ts,open,high,low,close,volume,schemaName\n"
            "SYN,2024-01-01 23:00:00+00:00,100,102,99,101,1200,ohlcv-1h\n"
            "SYN,2024-01-02 00:00:00+00:00,101,103,100,102,2300,ohlcv-1h\n";
  source.close();
  auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = root / "Source",
      .storeDirectory = root / "Data",
      .symbols = {"SYN"},
      .rowsPerSegment = 1,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  if (!built.ok()) {
    return built.error();
  }
  auto reader = bte::data::ReleaseSnapshotReader::open(
      root / "Data", built.value().snapshotId);
  if (!reader.ok()) {
    return reader.error();
  }
  const auto start = bte::core::time::fromUnixMillis(1'704'150'000'000);
  const auto end = bte::core::time::fromUnixMillis(1'704'157'200'000);
  auto selected = reader.value()->select(
      {.symbols = {"SYN"}, .range = {start, end}, .timeframe = "ohlcv-1h"});
  if (!selected.ok()) {
    return selected.error();
  }
  return bte::results::RunDescriptor{
      .universe = {"SYN"},
      .range = {start, end},
      .initialCapitalMicrodollars = 100'000'000,
      .strategyId = "starter",
      .strategyHash = std::string(64, 'c'),
      .dataSelection = selected.value().identity,
  };
}

bte::core::Result<void> run(const std::string &mode,
                            const std::filesystem::path &root,
                            const std::string &argument) {
  if (mode == "hold") {
    auto descriptor = createFixture(root);
    if (!descriptor.ok()) {
      return descriptor.error();
    }
    auto store = bte::results::ResultStore::open(root / "Store", root / "Data");
    if (!store.ok()) {
      return store.error();
    }
    auto writer = store.value()->begin(descriptor.value());
    if (!writer.ok()) {
      return writer.error();
    }
    const auto bars = std::stoi(argument);
    for (int bar = 0; bar < bars; ++bar) {
      auto appended = writer.value()->append({{
          .sequence = static_cast<std::uint64_t>(bar),
          .timestamp = descriptor.value().range.start + std::chrono::hours{bar},
          .symbol = "SYN",
          .family = bte::results::RecordFamily::portfolio,
          .cashMicrodollars = 100'000'000,
          .marketValueMicrodollars = 0,
          .equityMicrodollars = 100'000'000,
          .pnlMicrodollars = 0,
          .positionShares = 0,
      }});
      if (!appended.ok()) {
        return appended.error();
      }
    }
    std::cout << writer.value()->resultId() << std::endl;
    std::string stop;
    std::getline(std::cin, stop);
    return {};
  }
  auto store = bte::results::ResultStore::open(root / "Store", root / "Data");
  if (!store.ok()) {
    return store.error();
  }
  if (mode == "publish") {
    auto published = store.value()->publishDraft(argument);
    std::cout << (published.ok()
                      ? "published"
                      : published.error().code ==
                                bte::core::ErrorCode::resourceBusy
                            ? "busy"
                            : "error")
              << std::endl;
    return {};
  }
  auto drafts = store.value()->listDrafts();
  if (!drafts.ok()) {
    return drafts.error();
  }
  std::cout << drafts.value().size();
  if (mode == "recover") {
    for (const auto &draft : drafts.value()) {
      auto published = store.value()->publishDraft(draft.resultId);
      if (!published.ok()) {
        return published.error();
      }
      auto opened = store.value()->openResult(draft.resultId);
      if (!opened.ok()) {
        return opened.error();
      }
      std::cout << ' ' << static_cast<int>(opened.value().status) << ' '
                << opened.value().records.size();
    }
  }
  std::cout << std::endl;
  return {};
}

} // namespace

// argc/argv are the required C ABI entrypoint; all module calls use typed
// values.
int main(int argc, char **argv) {
  if (argc != 4) {
    return 2;
  }
  try {
    auto result = run(argv[1], std::filesystem::path{argv[2]}, argv[3]);
    if (!result.ok()) {
      std::cerr << result.error().message << '\n';
      return 1;
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
