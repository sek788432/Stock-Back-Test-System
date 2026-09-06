#include "Bte/Data/ReleaseSnapshot.h"

#include "Bte/Core/Result.h"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr auto usage =
    "Usage: bte_snapshot_builder <source-dir> <store-dir> <calendar-sha256> "
    "<split-sha256> <symbol> [symbol...]";

} // namespace

int run(const int argc, char **argv) {
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

int main(const int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  } catch (...) {
    std::cerr << "Unexpected snapshot builder failure\n";
  }
  return 1;
}
