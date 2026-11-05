#pragma once

#include <Bte/Core/Result.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bte::bindings::python {

struct RuntimePackage final {
  std::string name;
  std::string version;
  std::string artifactHash;

  bool operator==(const RuntimePackage &) const = default;
};

inline constexpr std::string_view managedRuntimeProfileId =
    "macos-arm64-cpython-3.14.9-stockbt-v1";

struct RuntimeProfile {
  std::filesystem::path root;
  std::filesystem::path pythonExecutablePath;
  std::filesystem::path timezoneDatabasePath;
  std::string profileId;
  std::string manifestHash;
  std::string pythonVersion;
  std::string pythonAbi;
  std::string stockbtApiVersion;
  std::string canonicalManifestJson;
  std::string stockbtSourceHash;
  std::vector<RuntimePackage> packages;
};

[[nodiscard]] core::Result<RuntimeProfile>
verifyRuntimeProfile(const std::filesystem::path &profileRoot);

[[nodiscard]] core::Result<RuntimeProfile>
discoverManagedRuntime(const std::filesystem::path &applicationResourcesRoot);

} // namespace bte::bindings::python
