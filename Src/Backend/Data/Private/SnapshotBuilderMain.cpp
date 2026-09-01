#include "Bte/Data/ReleaseSnapshot.h"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr auto usage =
    "Usage: bte_snapshot_builder <source-dir> <store-dir> <calendar-sha256> "
    "<split-sha256> <symbol> [symbol...]";

} // namespace

int main(const int argc, char **argv) {
  const auto arguments = std::span{argv, static_cast<std::size_t>(argc)};
  if (arguments.size() < 6) {
    std::cerr << usage << '\n';
    return 2;
  }

  std::vector<std::string> symbols;
  symbols.reserve(arguments.size() - 5);
  for (const auto *symbol : arguments.subspan(5)) {
    symbols.emplace_back(symbol);
  }

  const auto result = bte::data::buildReleaseSnapshot({
      .sourceDirectory = std::filesystem::path{arguments[1]},
      .storeDirectory = std::filesystem::path{arguments[2]},
      .symbols = std::move(symbols),
      .calendarHash = arguments[3],
      .splitManifestHash = arguments[4],
  });
  if (!result.ok()) {
    std::cerr << result.error().message << '\n';
    return 1;
  }
  std::cout << result.value().snapshotId << '\n';
  return 0;
}
