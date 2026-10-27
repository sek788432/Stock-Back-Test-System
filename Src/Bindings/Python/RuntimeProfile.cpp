#include "RuntimeProfile.h"

#include "StrategyProtocol.h"

#include <Bte/Core/Digest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bte::bindings::python {
namespace {

constexpr std::size_t maximumManifestBytes = 1024U * 1024U;

struct PinnedComponent final {
  std::string_view version;
  std::string_view artifactSha256;
  std::vector<std::string_view> licenses;
};

const std::map<std::string, PinnedComponent> &pinnedComponents() {
  static const std::map<std::string, PinnedComponent> pins{
      {"CPython",
       {"3.14.9",
        "226d696642a8a53d03b4d5b255b1c319b1575a6d44bd723a953c9f507755e36a",
        {"PSF-2.0"}}},
      {"numpy",
       {"2.5.4",
        "ffa6ce09a1c6a08e9667dd9c97aa0b14184e8d18f2a14b78b2a2328c9147f076",
        {"BSD-3-Clause"}}},
      {"pandas",
       {"3.0.6",
        "ff51a4459ed036e93d1eb1bb5e6e7b28685d3cb6b7c12b91c05b31024e234729",
        {"BSD-3-Clause"}}},
      {"python-dateutil",
       {"2.9.0.post0",
        "a8b2bc7bffae282281c8140a97d3aa9c14da0b136dfe83f850eea9a5f7470427",
        {"Apache-2.0", "BSD-3-Clause"}}},
      {"six",
       {"1.17.0",
        "4721f391ed90541fddacab5acf947aa0d3dc7d27b2e1e8eda2be8970586c3274",
        {"MIT"}}},
      {"tzdata",
       {"2026.5",
        "b683bd1b6659ddcd810ff02ad09ba821d4bf1065072805063eb35c49617905ac",
        {"Apache-2.0"}}},
  };
  return pins;
}

[[nodiscard]] core::Error unavailableError(std::string message) {
  return core::makeError(core::ErrorCode::runtimeUnavailable,
                         std::move(message));
}

[[nodiscard]] core::Error integrityError(std::string message) {
  return core::makeError(core::ErrorCode::runtimeIntegrityFailure,
                         std::move(message));
}

[[nodiscard]] bool isSha256(const std::string_view value) {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] bool hasExactKeys(const nlohmann::json &value,
                                const std::set<std::string> &keys) {
  if (!value.is_object() || value.size() != keys.size()) {
    return false;
  }
  return std::ranges::all_of(
      keys, [&value](const std::string &key) { return value.contains(key); });
}

[[nodiscard]] bool isSafeRelativePath(const std::string_view value) {
  if (value.empty() || value.front() == '/' ||
      value.find('\\') != std::string_view::npos) {
    return false;
  }
  const std::filesystem::path path{value};
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return false;
  }
  for (const auto &component : path) {
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
  }
  return path.generic_string() == value;
}

[[nodiscard]] bool isWithinRoot(const std::filesystem::path &root,
                                const std::filesystem::path &candidate) {
  auto rootPart = root.begin();
  auto candidatePart = candidate.begin();
  for (; rootPart != root.end(); ++rootPart, ++candidatePart) {
    if (candidatePart == candidate.end() || *rootPart != *candidatePart) {
      return false;
    }
  }
  return candidatePart != candidate.end();
}

#if defined(__unix__) || defined(__APPLE__)
class FileDescriptor final {
public:
  explicit FileDescriptor(const int value) : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      static_cast<void>(::close(value_));
    }
  }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  [[nodiscard]] int get() const noexcept { return value_; }

private:
  int value_;
};

class ReadOnlyMapping final {
public:
  ReadOnlyMapping(const void *address, const std::size_t length)
      : address_(address), length_(length) {}
  ~ReadOnlyMapping() {
    if (address_ != MAP_FAILED && length_ > 0) {
      static_cast<void>(::munmap(const_cast<void *>(address_), length_));
    }
  }
  ReadOnlyMapping(const ReadOnlyMapping &) = delete;
  ReadOnlyMapping &operator=(const ReadOnlyMapping &) = delete;
  [[nodiscard]] const void *get() const noexcept { return address_; }

private:
  const void *address_;
  std::size_t length_;
};

[[nodiscard]] core::Result<std::string>
sha256File(const std::filesystem::path &path) {
  FileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (file.get() < 0) {
    return integrityError("Could not open a managed runtime file.");
  }
  struct stat status {};
  if (::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 ||
      static_cast<std::uintmax_t>(status.st_size) >
          static_cast<std::uintmax_t>(
              std::numeric_limits<std::size_t>::max())) {
    return integrityError("A managed runtime entry is not a regular file.");
  }
  const auto size = static_cast<std::size_t>(status.st_size);
  if (size == 0) {
    return core::sha256(std::string_view{});
  }
  const auto *mapping =
      ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file.get(), 0);
  if (mapping == MAP_FAILED) {
    return integrityError(
        "Could not map a managed runtime file for verification.");
  }
  const ReadOnlyMapping mapped{mapping, size};
  return core::sha256(
      std::span{static_cast<const std::byte *>(mapped.get()), size});
}
#else
[[nodiscard]] core::Result<std::string>
sha256File(const std::filesystem::path &) {
  return unavailableError(
      "Managed runtime verification is unavailable on this platform.");
}
#endif

[[nodiscard]] bool isSupportedHost() {
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
  return true;
#else
  return false;
#endif
}

[[nodiscard]] core::Result<std::string>
readManifest(const std::filesystem::path &path) {
  std::error_code error;
  const auto fileSize = std::filesystem::file_size(path, error);
  if (error || fileSize > maximumManifestBytes) {
    return integrityError(
        "Runtime profile manifest is missing or exceeds 1 MiB.");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return integrityError("Runtime profile manifest could not be opened.");
  }
  std::string bytes(static_cast<std::size_t>(fileSize), '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!input || input.peek() != std::char_traits<char>::eof()) {
    return integrityError(
        "Runtime profile manifest could not be read consistently.");
  }
  return bytes;
}

[[nodiscard]] bool validComponent(const nlohmann::json &component) {
  static const std::set<std::string> componentKeys{
      "artifactSha256", "licenses", "name", "upstreamUrl", "version"};
  if (!hasExactKeys(component, componentKeys) ||
      !component.at("artifactSha256").is_string() ||
      !isSha256(
          component.at("artifactSha256").get_ref<const std::string &>()) ||
      !component.at("name").is_string() ||
      component.at("name").get_ref<const std::string &>().empty() ||
      !component.at("version").is_string() ||
      component.at("version").get_ref<const std::string &>().empty() ||
      !component.at("upstreamUrl").is_string() ||
      component.at("upstreamUrl").get_ref<const std::string &>().empty() ||
      !component.at("licenses").is_array() ||
      component.at("licenses").empty()) {
    return false;
  }
  return std::ranges::all_of(
      component.at("licenses"), [](const nlohmann::json &license) {
        return license.is_string() &&
               !license.get_ref<const std::string &>().empty();
      });
}

} // namespace

core::Result<RuntimeProfile>
verifyRuntimeProfile(const std::filesystem::path &profileRoot) {
  std::error_code error;
  const auto canonicalRoot = std::filesystem::canonical(profileRoot, error);
  if (error || !std::filesystem::is_directory(canonicalRoot)) {
    return integrityError("Runtime profile directory is missing or invalid.");
  }
  const auto manifestPath = canonicalRoot / "manifest.json";
  const auto canonicalManifestPath =
      std::filesystem::canonical(manifestPath, error);
  if (error || !isWithinRoot(canonicalRoot, canonicalManifestPath) ||
      !std::filesystem::is_regular_file(canonicalManifestPath)) {
    return integrityError(
        "Runtime profile manifest is missing or escapes its profile.");
  }
  const auto manifestBytes = readManifest(canonicalManifestPath);
  if (!manifestBytes.ok()) {
    return manifestBytes.error();
  }
  const auto manifest = parseCanonicalJson(manifestBytes.value());
  if (!manifest.ok()) {
    return integrityError(
        "Runtime profile manifest is not valid canonical JSON.");
  }
  const auto canonicalBytes = serializeCanonicalJson(manifest.value());
  if (!canonicalBytes.ok() || canonicalBytes.value() != manifestBytes.value()) {
    return integrityError("Runtime profile manifest bytes are not canonical.");
  }

  static const std::set<std::string> manifestKeys{"architecture",
                                                  "components",
                                                  "installedFiles",
                                                  "platform",
                                                  "profileBuilderVersion",
                                                  "profileId",
                                                  "pythonAbi",
                                                  "pythonExecutable",
                                                  "pythonVersion",
                                                  "schemaVersion",
                                                  "stockbtApiVersion",
                                                  "stockbtSourceSha256"};
  const auto &value = manifest.value();
  if (!hasExactKeys(value, manifestKeys) ||
      !value.at("schemaVersion").is_number_unsigned() ||
      value.at("schemaVersion").get<std::uint64_t>() != 1 ||
      value.at("profileId") != std::string{managedRuntimeProfileId} ||
      value.at("platform") != "macos" || value.at("architecture") != "arm64" ||
      value.at("pythonVersion") != "3.14.9" ||
      value.at("pythonAbi") != "cp314" ||
      value.at("stockbtApiVersion") != "1" ||
      !value.at("profileBuilderVersion").is_string() ||
      value.at("profileBuilderVersion")
          .get_ref<const std::string &>()
          .empty() ||
      !value.at("stockbtSourceSha256").is_string() ||
      !isSha256(
          value.at("stockbtSourceSha256").get_ref<const std::string &>()) ||
      !value.at("components").is_array() || value.at("components").empty() ||
      !value.at("installedFiles").is_array() ||
      value.at("installedFiles").empty() ||
      !value.at("pythonExecutable").is_string()) {
    return integrityError(
        "Runtime profile manifest has unsupported or missing metadata.");
  }

  std::set<std::string> componentNames;
  for (const auto &component : value.at("components")) {
    if (!validComponent(component)) {
      return integrityError(
          "Runtime profile component metadata is invalid or duplicated.");
    }
    const auto name = component.at("name").get<std::string>();
    if (!componentNames.insert(name).second) {
      return integrityError(
          "Runtime profile component metadata is invalid or duplicated.");
    }
    const auto &artifactDigest =
        component.at("artifactSha256").get_ref<const std::string &>();
    const auto &version =
        component.at("version").get_ref<const std::string &>();
    const auto &licenses = component.at("licenses");
    if (const auto pin = pinnedComponents().find(name);
        pin != pinnedComponents().end()) {
      std::set<std::string> actualLicenses;
      for (const auto &license : licenses) {
        actualLicenses.insert(license.get<std::string>());
      }
      const std::set<std::string> approvedLicenses(pin->second.licenses.begin(),
                                                   pin->second.licenses.end());
      if (version != pin->second.version ||
          artifactDigest != pin->second.artifactSha256 ||
          actualLicenses.size() != licenses.size() ||
          actualLicenses != approvedLicenses) {
        return integrityError(
            "Runtime profile component differs from its accepted pin.");
      }
    } else if (name == "stockbt") {
      if (version != "1" ||
          artifactDigest !=
              value.at("stockbtSourceSha256").get_ref<const std::string &>() ||
          licenses.size() != 1 || licenses.front() != "Apache-2.0") {
        return integrityError(
            "Runtime profile stockbt component metadata is invalid.");
      }
    } else {
      return integrityError(
          "Runtime profile includes an unapproved component.");
    }
  }
  if (componentNames.size() != pinnedComponents().size() + 1 ||
      !componentNames.contains("stockbt") ||
      !std::ranges::all_of(pinnedComponents(),
                           [&componentNames](const auto &pin) {
                             return componentNames.contains(pin.first);
                           })) {
    return integrityError("Runtime profile is missing a pinned component.");
  }

  const auto pythonExecutable = value.at("pythonExecutable").get<std::string>();
  if (!isSafeRelativePath(pythonExecutable)) {
    return integrityError(
        "Runtime interpreter path is not a safe relative path.");
  }
  static const std::set<std::string> installedFileKeys{"path", "sha256"};
  std::set<std::string> installedPaths;
  bool interpreterIsListed = false;
  std::optional<std::filesystem::path> timezoneDatabaseRelativePath;
  for (const auto &file : value.at("installedFiles")) {
    if (!hasExactKeys(file, installedFileKeys) ||
        !file.at("path").is_string() || !file.at("sha256").is_string()) {
      return integrityError("Runtime installed-file entry is malformed.");
    }
    const auto relativePath = file.at("path").get<std::string>();
    const auto expectedHash = file.at("sha256").get<std::string>();
    if (!isSafeRelativePath(relativePath) || !isSha256(expectedHash) ||
        !installedPaths.insert(relativePath).second) {
      return integrityError(
          "Runtime installed-file path or digest is invalid.");
    }
    const auto candidate = canonicalRoot / std::filesystem::path{relativePath};
    const auto canonicalCandidate =
        std::filesystem::canonical(candidate, error);
    if (error || !isWithinRoot(canonicalRoot, canonicalCandidate) ||
        !std::filesystem::is_regular_file(canonicalCandidate)) {
      return integrityError(
          "A runtime file is missing or resolves outside its profile.");
    }
    const auto actualHash = sha256File(canonicalCandidate);
    if (!actualHash.ok()) {
      return actualHash.error();
    }
    if (actualHash.value() != expectedHash) {
      return integrityError(
          "A managed runtime installed-file hash does not match.");
    }
    if (relativePath.ends_with("tzdata/zoneinfo/tzdata.zi")) {
      timezoneDatabaseRelativePath =
          std::filesystem::path{relativePath}.parent_path();
    }
    interpreterIsListed =
        interpreterIsListed || relativePath == pythonExecutable;
  }
  if (!interpreterIsListed || !timezoneDatabaseRelativePath.has_value()) {
    return integrityError("Runtime interpreter or pinned timezone database is "
                          "not covered by installed-file hashes.");
  }

  std::set<std::string> actualFiles;
  for (std::filesystem::recursive_directory_iterator
           iterator(canonicalRoot, error),
       end;
       iterator != end && !error; iterator.increment(error)) {
    const auto relativePath =
        iterator->path().lexically_relative(canonicalRoot);
    if (relativePath.empty()) {
      return integrityError(
          "Runtime profile entry could not be made relative to its root.");
    }
    const auto relative = relativePath.generic_string();
    if (relative == "manifest.json") {
      continue;
    }
    const auto status = iterator->symlink_status(error);
    if (error) {
      break;
    }
    if (std::filesystem::is_directory(status)) {
      continue;
    }
    if (!std::filesystem::is_regular_file(status) &&
        !std::filesystem::is_symlink(status)) {
      return integrityError(
          "Runtime profile contains an unsupported filesystem entry.");
    }
    actualFiles.insert(relative);
  }
  if (error || actualFiles != installedPaths) {
    return integrityError(
        "Runtime profile files differ from the installed-file manifest.");
  }

  const auto executablePath =
      std::filesystem::canonical(canonicalRoot / pythonExecutable, error);
  if (error || !isWithinRoot(canonicalRoot, executablePath)) {
    return integrityError("Managed runtime interpreter path is invalid.");
  }
  const auto permissions =
      std::filesystem::status(executablePath, error).permissions();
  if (error || (permissions & (std::filesystem::perms::owner_exec |
                               std::filesystem::perms::group_exec |
                               std::filesystem::perms::others_exec)) ==
                   std::filesystem::perms::none) {
    return integrityError("Managed runtime interpreter is not executable.");
  }
  const auto timezoneDatabasePath = std::filesystem::canonical(
      canonicalRoot / *timezoneDatabaseRelativePath, error);
  if (error || !isWithinRoot(canonicalRoot, timezoneDatabasePath) ||
      !std::filesystem::is_directory(timezoneDatabasePath)) {
    return integrityError("Managed timezone database path is invalid.");
  }

  std::vector<RuntimePackage> packages;
  packages.reserve(value.at("components").size());
  for (const auto &component : value.at("components")) {
    packages.push_back({
        .name = component.at("name").get<std::string>(),
        .version = component.at("version").get<std::string>(),
        .artifactHash = component.at("artifactSha256").get<std::string>(),
    });
  }
  return RuntimeProfile{
      .root = canonicalRoot,
      .pythonExecutablePath = executablePath,
      .timezoneDatabasePath = timezoneDatabasePath,
      .profileId = std::string{managedRuntimeProfileId},
      .manifestHash = core::sha256(manifestBytes.value()),
      .pythonVersion = "3.14.9",
      .pythonAbi = "cp314",
      .stockbtApiVersion = "1",
      .canonicalManifestJson = manifestBytes.value(),
      .stockbtSourceHash = value.at("stockbtSourceSha256").get<std::string>(),
      .packages = std::move(packages),
  };
}

core::Result<RuntimeProfile>
discoverManagedRuntime(const std::filesystem::path &applicationResourcesRoot) {
  if (!isSupportedHost()) {
    return unavailableError("Python Strategy is unavailable on this platform.");
  }
  const auto profileRoot = applicationResourcesRoot / "RuntimeProfiles" /
                           std::string{managedRuntimeProfileId};
  std::error_code error;
  if (!std::filesystem::exists(profileRoot, error) || error) {
    return unavailableError(
        "The managed Python runtime profile is not installed.");
  }
  const auto profile = verifyRuntimeProfile(profileRoot);
  if (!profile.ok()) {
    return profile.error();
  }
  return profile.value();
}

} // namespace bte::bindings::python
