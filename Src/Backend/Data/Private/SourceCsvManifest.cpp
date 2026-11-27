#include "SourceCsvManifest.h"

#include <algorithm>
#include <array>
#include <cctype> // IWYU pragma: keep
#include <fstream>
#include <functional> // IWYU pragma: keep
#include <ranges>     // IWYU pragma: keep
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bte::data::detail {
namespace {

constexpr std::string_view sourceManifestMagic = "BTE-SOURCE-MANIFEST-V1";
constexpr std::string_view sourceManifestName = "SourceManifest.btesource";

std::string trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return std::string{value};
}

std::vector<std::string> split(std::string_view text, const char delimiter) {
  std::vector<std::string> fields;
  while (true) {
    const auto position = text.find(delimiter);
    if (position == std::string_view::npos) {
      fields.push_back(trim(text));
      break;
    }
    fields.push_back(trim(text.substr(0, position)));
    text.remove_prefix(position + 1);
  }
  return fields;
}

bool validHash(const std::string &hash) {
  return hash.size() == 64 &&
         std::ranges::all_of(hash, [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

bool validSymbol(const std::string_view symbol) {
  const auto first = symbol.empty() ? '\0' : symbol.front();
  const auto startsWithAllowedCharacter = (first >= 'A' && first <= 'Z') ||
                                          (first >= 'a' && first <= 'z') ||
                                          (first >= '0' && first <= '9');
  if (symbol.empty() || !startsWithAllowedCharacter) {
    return false;
  }
  return std::ranges::all_of(symbol, [](const char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '.' ||
           character == '_' || character == '-';
  });
}

} // namespace

core::Result<SourceManifest>
readSourceManifest(const std::filesystem::path &sourceDirectory) {
  const auto manifestPath = sourceDirectory / std::string{sourceManifestName};
  auto fileExistsError = std::error_code{};
  const auto fileExists =
      std::filesystem::exists(manifestPath, fileExistsError);
  if (fileExistsError) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Unable to inspect source manifest: " +
                               manifestPath.string());
  }
  if (!fileExists) {
    return SourceManifest{};
  }

  std::ifstream input{manifestPath, std::ios::binary};
  std::string line;
  if (!input || !std::getline(input, line) ||
      trim(line) != sourceManifestMagic) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Source manifest header is invalid: " +
                               manifestPath.string());
  }

  auto sourceManifest = SourceManifest{.present = true, .files = {}};
  constexpr std::array<std::string_view, 5> supportedTimeframes{
      "ohlcv-1s", "ohlcv-1m", "ohlcv-1h", "ohlcv-1d", "ohlcv-eod"};
  while (std::getline(input, line)) {
    const auto entry = trim(line);
    if (!entry.starts_with("file=")) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Source manifest entry is invalid");
    }
    const auto fields = split(
        std::string_view{entry}.substr(std::string_view{"file="}.size()), '|');
    if (fields.size() != 3 || fields[0].size() <= 4 ||
        !fields[0].ends_with(".csv") ||
        !std::ranges::all_of(fields[0],
                             [](const char character) {
                               return (character >= 'a' && character <= 'z') ||
                                      (character >= 'A' && character <= 'Z') ||
                                      (character >= '0' && character <= '9') ||
                                      character == '.' || character == '_' ||
                                      character == '-';
                             }) ||
        std::ranges::find(supportedTimeframes, fields[1]) ==
            supportedTimeframes.end() ||
        !validHash(fields[2])) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Source manifest metadata is invalid");
    }
    const auto insertion = sourceManifest.files.emplace(
        fields[0],
        SourceFileDeclaration{.timeframe = fields[1], .sha256 = fields[2]});
    if (!insertion.second) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Source manifest contains a duplicate file");
    }
  }
  if (input.bad()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Unable to read source manifest: " +
                               manifestPath.string());
  }
  return sourceManifest;
}

std::string sourceCsvFilenameForSymbol(std::string symbol) {
  std::ranges::replace(symbol, '.', '_');
  symbol.append(".csv");
  return symbol;
}

core::Result<std::filesystem::path>
sourceCsvPathForSymbol(const std::filesystem::path &sourceDirectory,
                       const std::string &symbol,
                       const SourceManifest &sourceManifest) {
  if (!validSymbol(symbol)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "CSV source symbol is invalid");
  }
  const auto exactFilename = symbol + ".csv";
  const auto extractedFilename = sourceCsvFilenameForSymbol(symbol);
  const auto exactPath = sourceDirectory / exactFilename;
  const auto extractedPath = sourceDirectory / extractedFilename;
  if (exactFilename == extractedFilename) {
    if (sourceManifest.present &&
        !sourceManifest.files.contains(exactFilename)) {
      return core::makeError(core::ErrorCode::notFound,
                             "CSV source is not declared for symbol '" +
                                 symbol + "'");
    }
    return exactPath;
  }

  const auto exactIsDeclared = sourceManifest.files.contains(exactFilename);
  const auto extractedIsDeclared =
      sourceManifest.files.contains(extractedFilename);
  if (sourceManifest.present) {
    if (exactIsDeclared) {
      return exactPath;
    }
    if (extractedIsDeclared) {
      return extractedPath;
    }
    return core::makeError(core::ErrorCode::notFound,
                           "CSV source is not declared for symbol '" + symbol +
                               "'");
  }

  auto filesystemError = std::error_code{};
  if (std::filesystem::exists(exactPath, filesystemError)) {
    return exactPath;
  }
  if (filesystemError) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Unable to inspect snapshot source: " +
                               exactPath.string());
  }
  if (std::filesystem::exists(extractedPath, filesystemError)) {
    return extractedPath;
  }
  if (filesystemError) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Unable to inspect snapshot source: " +
                               extractedPath.string());
  }
  return exactPath;
}

} // namespace bte::data::detail
