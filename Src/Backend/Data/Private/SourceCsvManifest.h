#pragma once

#include "Bte/Core/Result.h"

#include <filesystem>
#include <map>
#include <string>

namespace bte::data::detail {

struct SourceFileDeclaration final {
  std::string timeframe;
  std::string sha256;
};

struct SourceManifest final {
  bool present = false;
  std::map<std::string, SourceFileDeclaration> files;
};

[[nodiscard]] core::Result<SourceManifest>
readSourceManifest(const std::filesystem::path &sourceDirectory);

[[nodiscard]] core::Result<std::filesystem::path>
sourceCsvPathForSymbol(const std::filesystem::path &sourceDirectory,
                       const std::string &symbol,
                       const SourceManifest &sourceManifest);

[[nodiscard]] std::string sourceCsvFilenameForSymbol(std::string symbol);

} // namespace bte::data::detail
