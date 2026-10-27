#include "Bte/Results/ReproducibilityStore.h"

#include "Bte/Core/CanonicalJson.h"
#include "Bte/Core/Digest.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace bte::results {
namespace {

constexpr std::size_t maximumEvidenceBytes = std::size_t{64} * 1024U;
constexpr std::size_t maximumStrategySourceBytes = std::size_t{1024} * 1024U;
constexpr std::size_t maximumErrorCodeBytes = 128;
constexpr std::size_t maximumErrorMessageBytes = 2048;
constexpr std::size_t requiredAttemptCount = 3;
constexpr std::string_view numericPolicy = "fixed-point-v1";
constexpr std::string_view aggregationPolicy = "utcCalendarDayV1";

std::mutex &storeMutex() {
  static std::mutex mutex;
  return mutex;
}

core::Error fileError(std::string operation, std::string_view detail);

class VerificationFileLock final {
public:
  VerificationFileLock() = default;
  ~VerificationFileLock() { release(); }

  VerificationFileLock(const VerificationFileLock &) = delete;
  VerificationFileLock &operator=(const VerificationFileLock &) = delete;
  VerificationFileLock(VerificationFileLock &&other) noexcept {
#if defined(__APPLE__) || defined(__linux__)
    descriptor_ = std::exchange(other.descriptor_, -1);
#else
    static_cast<void>(other);
#endif
  }
  VerificationFileLock &operator=(VerificationFileLock &&other) noexcept {
    if (this != &other) {
      release();
#if defined(__APPLE__) || defined(__linux__)
      descriptor_ = std::exchange(other.descriptor_, -1);
#else
      static_cast<void>(other);
#endif
    }
    return *this;
  }

  [[nodiscard]] static core::Result<VerificationFileLock>
  acquire(const std::filesystem::path &directory) {
    VerificationFileLock lock;
#if defined(__APPLE__) || defined(__linux__)
    const auto path = (directory / ".verification-store.lock").string();
    lock.descriptor_ =
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): lock O_CREAT mode
        ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (lock.descriptor_ < 0) {
      return fileError(
          "Unable to open verification store lock",
          std::error_code{errno, std::generic_category()}.message());
    }
    if (::flock(lock.descriptor_, LOCK_EX) != 0) {
      const auto savedError = errno;
      return fileError(
          "Unable to lock verification store",
          std::error_code{savedError, std::generic_category()}.message());
    }
#else
    static_cast<void>(directory);
#endif
    return lock;
  }

private:
  void release() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    if (descriptor_ >= 0) {
      static_cast<void>(::flock(descriptor_, LOCK_UN));
      static_cast<void>(::close(descriptor_));
      descriptor_ = -1;
    }
#endif
  }

#if defined(__APPLE__) || defined(__linux__)
  int descriptor_ = -1;
#endif
};

core::Error invalidArgument(std::string message) {
  return core::makeError(core::ErrorCode::invalidArgument, std::move(message));
}

core::Error schemaError(std::string message) {
  return core::makeError(core::ErrorCode::schemaMismatch, std::move(message));
}

core::Error fileError(std::string operation, const std::string_view detail) {
  operation.append(": ");
  operation.append(detail);
  return core::makeError(core::ErrorCode::permissionDenied,
                         std::move(operation));
}

bool validHex(const std::string_view value, const std::size_t length) {
  return value.size() == length &&
         std::ranges::all_of(value, [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

bool validIdentity(const std::string_view value) { return validHex(value, 32); }

std::string randomIdentity() {
  std::random_device source;
  constexpr auto digits = std::string_view{"0123456789abcdef"};
  std::string result(32, '0');
  for (auto &character : result) {
    character = digits[source() & 0xfU];
  }
  return result;
}

std::string_view runStatusName(const RunStatus status) {
  switch (status) {
  case RunStatus::running:
    return "running";
  case RunStatus::completed:
    return "completed";
  case RunStatus::failed:
    return "failed";
  case RunStatus::canceled:
    return "canceled";
  case RunStatus::interrupted:
    return "interrupted";
  case RunStatus::incomplete:
    return "incomplete";
  }
  return {};
}

std::optional<RunStatus> parseRunStatus(const std::string_view status) {
  if (status == "running") {
    return RunStatus::running;
  }
  if (status == "completed") {
    return RunStatus::completed;
  }
  if (status == "failed") {
    return RunStatus::failed;
  }
  if (status == "canceled") {
    return RunStatus::canceled;
  }
  if (status == "interrupted") {
    return RunStatus::interrupted;
  }
  if (status == "incomplete") {
    return RunStatus::incomplete;
  }
  return std::nullopt;
}

std::string_view verificationStatusName(const VerificationStatus status) {
  switch (status) {
  case VerificationStatus::running:
    return "running";
  case VerificationStatus::verified:
    return "verified";
  case VerificationStatus::mismatch:
    return "mismatch";
  case VerificationStatus::failed:
    return "failed";
  case VerificationStatus::canceled:
    return "canceled";
  case VerificationStatus::interrupted:
    return "interrupted";
  }
  return {};
}

std::optional<VerificationStatus>
parseVerificationStatus(const std::string_view status) {
  if (status == "running") {
    return VerificationStatus::running;
  }
  if (status == "verified") {
    return VerificationStatus::verified;
  }
  if (status == "mismatch") {
    return VerificationStatus::mismatch;
  }
  if (status == "failed") {
    return VerificationStatus::failed;
  }
  if (status == "canceled") {
    return VerificationStatus::canceled;
  }
  if (status == "interrupted") {
    return VerificationStatus::interrupted;
  }
  return std::nullopt;
}

core::Result<void> validateFunctionalRunInputs(
    const RunDescriptor &descriptor,
    const std::vector<std::string> &orderedUniverse,
    const std::array<std::string_view, 13> &availabilityKeys) {
  if (descriptor.schemaVersion != 3 || descriptor.universe.empty() ||
      descriptor.range.start >= descriptor.range.end ||
      descriptor.initialCapitalMicrodollars <= 0 ||
      descriptor.strategyId.empty() ||
      descriptor.provenance.strategyMode != StrategyMode::python ||
      std::ranges::adjacent_find(orderedUniverse) != orderedUniverse.end() ||
      !validHex(descriptor.dataSelection.snapshotId, 64) ||
      !validHex(descriptor.dataSelection.calendarHash, 64) ||
      !validHex(descriptor.dataSelection.splitManifestHash, 64) ||
      descriptor.dataSelection.timeframe.empty() ||
      descriptor.dataSelection.profile.empty()) {
    return invalidArgument(
        "Reproducibility verification requires a valid schema-3 Python Run");
  }
  const auto &provenance = descriptor.provenance;
  if (!provenance.engineBuild.has_value() || provenance.engineBuild->empty() ||
      provenance.timeframe.empty() ||
      provenance.timeframe != descriptor.dataSelection.timeframe ||
      !provenance.costProfile.has_value() ||
      !provenance.commissionRatePartsPerBillion.has_value() ||
      !provenance.slippageRatePartsPerBillion.has_value() ||
      !provenance.seed.has_value() || provenance.apiVersion.empty() ||
      provenance.templateVersion.empty() || !provenance.runtime.has_value() ||
      provenance.sourceUtf8.empty() ||
      provenance.sourceUtf8.size() > maximumStrategySourceBytes ||
      provenance.sourceUtf8.find('\0') != std::string::npos ||
      provenance.sourceUtf8.find('\r') != std::string::npos ||
      provenance.sourceUtf8.starts_with("\xef\xbb\xbf") ||
      provenance.costProfile->empty() ||
      *provenance.commissionRatePartsPerBillion < 0 ||
      *provenance.slippageRatePartsPerBillion < 0 || *provenance.seed < 0 ||
      provenance.sourceHash != core::sha256(provenance.sourceUtf8) ||
      descriptor.strategyHash != provenance.sourceHash ||
      !validHex(provenance.sourceHash, 64) ||
      provenance.provenanceAvailability.size() != availabilityKeys.size() ||
      !std::ranges::all_of(provenance.provenanceAvailability,
                           [](const auto &entry) {
                             return entry.second ==
                                    ProvenanceAvailabilityState::recorded;
                           }) ||
      !std::ranges::all_of(availabilityKeys, [&provenance](const auto key) {
        return provenance.provenanceAvailability.contains(std::string{key});
      })) {
    return invalidArgument(
        "Reproducibility verification inputs are incomplete");
  }
  return {};
}

struct CanonicalObjectParseMessages final {
  std::string_view invalid;
  std::string_view nonCanonical;
};

core::Result<nlohmann::json>
parseCanonicalObject(const std::string &jsonText,
                     const CanonicalObjectParseMessages messages) {
  auto parsed = core::parseCanonicalJson(jsonText);
  if (!parsed.ok() || !parsed.value().is_object()) {
    return invalidArgument(std::string{messages.invalid});
  }
  auto canonical = core::serializeCanonicalJson(parsed.value());
  if (!canonical.ok() || canonical.value() != jsonText) {
    return invalidArgument(std::string{messages.nonCanonical});
  }
  return parsed;
}

core::Result<nlohmann::json>
parseRuntimeManifest(const RuntimeProfileProvenance &runtime) {
  auto parsed = core::parseCanonicalJson(runtime.canonicalManifestJson);
  if (!parsed.ok() || !parsed.value().is_object() ||
      !validHex(runtime.manifestHash, 64) ||
      runtime.manifestHash != core::sha256(runtime.canonicalManifestJson) ||
      runtime.profileId.empty() || runtime.pythonVersion.empty() ||
      runtime.pythonAbi.empty() || !validHex(runtime.stockbtSourceHash, 64)) {
    return invalidArgument(
        "Reproducibility Runtime Profile identity is invalid");
  }
  auto canonical = core::serializeCanonicalJson(parsed.value());
  if (!canonical.ok() || canonical.value() != runtime.canonicalManifestJson) {
    return invalidArgument(
        "Reproducibility Runtime Profile manifest is not canonical");
  }
  return parsed;
}

core::Result<nlohmann::json>
runtimePackageValues(const RuntimeProfileProvenance &runtime,
                     const nlohmann::json &manifest) {
  const auto stringFieldMatches = [&manifest](const std::string_view key,
                                              const std::string_view expected) {
    const auto field = manifest.find(std::string{key});
    return field != manifest.end() && field->is_string() &&
           field->get_ref<const std::string &>() == expected;
  };
  const auto components = manifest.find("components");
  if (!stringFieldMatches("profileId", runtime.profileId) ||
      !stringFieldMatches("pythonVersion", runtime.pythonVersion) ||
      !stringFieldMatches("pythonAbi", runtime.pythonAbi) ||
      !stringFieldMatches("stockbtSourceSha256", runtime.stockbtSourceHash) ||
      components == manifest.end() || !components->is_array() ||
      components->size() != runtime.packages.size()) {
    return invalidArgument(
        "Reproducibility Runtime Profile does not match its manifest");
  }
  auto packages = runtime.packages;
  std::ranges::sort(packages, {}, &RuntimePackageIdentity::name);
  nlohmann::json packageValues = nlohmann::json::array();
  std::string previousPackage;
  for (const auto &package : packages) {
    if (package.name.empty() || package.name == previousPackage ||
        package.version.empty() || !validHex(package.artifactHash, 64)) {
      return invalidArgument(
          "Reproducibility Runtime Profile packages are invalid");
    }
    previousPackage = package.name;
    packageValues.push_back({{"artifactHash", package.artifactHash},
                             {"name", package.name},
                             {"version", package.version}});
  }
  std::vector<RuntimePackageIdentity> manifestPackages;
  for (const auto &component : *components) {
    if (!component.is_object() || !component.contains("name") ||
        !component.at("name").is_string() || !component.contains("version") ||
        !component.at("version").is_string() ||
        !component.contains("artifactSha256") ||
        !component.at("artifactSha256").is_string()) {
      return invalidArgument(
          "Reproducibility Runtime Profile package metadata is invalid");
    }
    manifestPackages.push_back(
        {.name = component.at("name").get<std::string>(),
         .version = component.at("version").get<std::string>(),
         .artifactHash = component.at("artifactSha256").get<std::string>()});
  }
  std::ranges::sort(manifestPackages, {}, &RuntimePackageIdentity::name);
  if (manifestPackages != packages ||
      std::ranges::none_of(packages, [&runtime](const auto &package) {
        return package.name == "stockbt" &&
               package.artifactHash == runtime.stockbtSourceHash;
      })) {
    return invalidArgument(
        "Reproducibility Runtime Profile packages are inconsistent");
  }
  return packageValues;
}

core::Result<nlohmann::json>
dataSpanIdentityValues(const RunDescriptor &descriptor,
                       const std::vector<std::string> &orderedUniverse) {
  nlohmann::json spans = nlohmann::json::array();
  std::size_t previousOrdinal = 0;
  bool hasPreviousOrdinal = false;
  for (const auto &span : descriptor.dataSelection.spans) {
    if ((hasPreviousOrdinal && previousOrdinal >= span.segmentOrdinal) ||
        !std::ranges::binary_search(orderedUniverse, span.symbol) ||
        !validHex(span.segmentId, 64) || span.segmentHash != span.segmentId ||
        span.rowCount == 0 || span.firstTimestamp > span.lastTimestamp) {
      return invalidArgument(
          "Reproducibility data-selection spans are invalid");
    }
    previousOrdinal = span.segmentOrdinal;
    hasPreviousOrdinal = true;
    spans.push_back(
        {{"firstRow", std::to_string(span.firstRow)},
         {"firstTimestampUnixMillis",
          std::to_string(core::time::toUnixMillis(span.firstTimestamp))},
         {"lastTimestampUnixMillis",
          std::to_string(core::time::toUnixMillis(span.lastTimestamp))},
         {"manifestOrdinal", std::to_string(span.segmentOrdinal)},
         {"rowCount", std::to_string(span.rowCount)},
         {"segmentHash", span.segmentHash},
         {"segmentId", span.segmentId},
         {"symbol", span.symbol}});
  }
  if (spans.empty()) {
    return invalidArgument(
        "Reproducibility verification requires immutable data spans");
  }
  return spans;
}

core::Result<VerificationIdentity>
functionalInputIdentity(const RunDescriptor &descriptor) {
  auto orderedUniverse = descriptor.universe;
  std::ranges::sort(orderedUniverse);
  static constexpr std::array<std::string_view, 13> availabilityKeys{
      "aggregationPolicy", "costs",         "dataSelection",
      "engineBuild",       "numericPolicy", "runtimeProfile",
      "runtimePackages",   "seed",          "strategyApi",
      "strategyArtifact",  "strategyMode",  "strategyTemplate",
      "timeframe"};
  const auto inputValidation = validateFunctionalRunInputs(
      descriptor, orderedUniverse, availabilityKeys);
  if (!inputValidation.ok()) {
    return inputValidation.error();
  }
  const auto &provenance = descriptor.provenance;
  auto parameters = parseCanonicalObject(
      provenance.canonicalParametersJson,
      {.invalid = "Reproducibility parameters must be a canonical JSON object",
       .nonCanonical = "Reproducibility parameters are not canonical JSON"});
  if (!parameters.ok()) {
    return parameters.error();
  }
  const auto runtime = provenance.runtime.value_or(RuntimeProfileProvenance{});
  auto manifest = parseRuntimeManifest(runtime);
  if (!manifest.ok()) {
    return manifest.error();
  }
  auto packageValues = runtimePackageValues(runtime, manifest.value());
  if (!packageValues.ok()) {
    return packageValues.error();
  }
  auto spans = dataSpanIdentityValues(descriptor, orderedUniverse);
  if (!spans.ok()) {
    return spans.error();
  }
  const auto optionalInteger = [](const std::optional<std::int64_t> value) {
    return value.has_value() ? nlohmann::json(std::to_string(*value))
                             : nlohmann::json(nullptr);
  };
  const nlohmann::json identity{
      {"aggregationPolicy", aggregationPolicy},
      {"dataSelection",
       {{"aggregationProfile", descriptor.dataSelection.profile},
        {"calendarHash", descriptor.dataSelection.calendarHash},
        {"snapshotId", descriptor.dataSelection.snapshotId},
        {"sourceTimeframe", descriptor.dataSelection.timeframe},
        {"spans", std::move(spans).value()},
        {"splitManifestHash", descriptor.dataSelection.splitManifestHash}}},
      {"engineBuild", provenance.engineBuild.value_or(std::string{})},
      {"initialCapitalMicrodollars",
       std::to_string(descriptor.initialCapitalMicrodollars)},
      {"numericPolicy", numericPolicy},
      {"rangeEndUnixMillis",
       std::to_string(core::time::toUnixMillis(descriptor.range.end))},
      {"rangeStartUnixMillis",
       std::to_string(core::time::toUnixMillis(descriptor.range.start))},
      {"runtime",
       {{"manifestHash", runtime.manifestHash},
        {"packages", std::move(packageValues).value()},
        {"profileId", runtime.profileId},
        {"pythonAbi", runtime.pythonAbi},
        {"pythonVersion", runtime.pythonVersion},
        {"stockbtSourceHash", runtime.stockbtSourceHash}}},
      {"schemaVersion", 3},
      {"seed", optionalInteger(provenance.seed)},
      {"strategy",
       {{"apiVersion", provenance.apiVersion},
        {"canonicalParameters", std::move(parameters).value()},
        {"mode", "python"},
        {"sourceHash", provenance.sourceHash},
        {"strategyHash", descriptor.strategyHash},
        {"strategyId", descriptor.strategyId},
        {"templateVersion", provenance.templateVersion}}},
      {"costs",
       {{"commissionRatePartsPerBillion",
         optionalInteger(provenance.commissionRatePartsPerBillion)},
        {"costProfile", provenance.costProfile.value_or(std::string{})},
        {"slippageRatePartsPerBillion",
         optionalInteger(provenance.slippageRatePartsPerBillion)}}},
      {"timeframe", provenance.timeframe},
      {"universe", descriptor.universe},
      {"verificationIdentityVersion", 1},
  };
  auto serialized = core::serializeCanonicalJson(identity);
  if (!serialized.ok()) {
    return serialized.error();
  }
  const auto identityHash = core::sha256(serialized.value());
  return VerificationIdentity{.canonicalJson = std::move(serialized).value(),
                              .sha256 = identityHash};
}

bool isObjectWithKeys(const nlohmann::json &value,
                      const std::set<std::string> &keys) {
  if (!value.is_object() || value.size() != keys.size()) {
    return false;
  }
  return std::ranges::all_of(
      keys, [&value](const std::string &key) { return value.contains(key); });
}

nlohmann::json attemptJson(const VerificationAttempt &attempt) {
  nlohmann::json error = nullptr;
  if (attempt.error.has_value()) {
    error = {{"code", attempt.error->code},
             {"message", attempt.error->message}};
  }
  return {
      {"attemptOrdinal", attempt.ordinal},
      {"canonicalResultHash", attempt.canonicalResultHash.has_value()
                                  ? nlohmann::json(*attempt.canonicalResultHash)
                                  : nlohmann::json(nullptr)},
      {"error", std::move(error)},
      {"resultId", attempt.resultId.has_value()
                       ? nlohmann::json(*attempt.resultId)
                       : nlohmann::json(nullptr)},
      {"terminalStatus",
       attempt.terminalStatus.has_value()
           ? nlohmann::json(std::string{runStatusName(*attempt.terminalStatus)})
           : nlohmann::json(nullptr)}};
}

core::Result<nlohmann::json> evidenceJson(VerificationEvidence &evidence,
                                          const bool includeIntegrity) {
  auto identity = core::parseCanonicalJson(evidence.identity.canonicalJson);
  if (!identity.ok() || !identity.value().is_object()) {
    return schemaError("Verification functional-input identity is invalid");
  }
  nlohmann::json attempts = nlohmann::json::array();
  for (const auto &attempt : evidence.attempts) {
    attempts.push_back(attemptJson(attempt));
  }
  nlohmann::json result{
      {"attempts", std::move(attempts)},
      {"identity", std::move(identity).value()},
      {"identityHash", evidence.identity.sha256},
      {"progressOrdinal", evidence.progressOrdinal},
      {"schemaVersion", 1},
      {"state", verificationStatusName(evidence.status)},
      {"verificationId", evidence.verificationId},
  };
  if (includeIntegrity) {
    result["integrityHash"] = evidence.integrityHash;
  }
  return result;
}

core::Result<std::string> evidenceBytes(VerificationEvidence &evidence) {
  auto withoutIntegrity = evidenceJson(evidence, false);
  if (!withoutIntegrity.ok()) {
    return withoutIntegrity.error();
  }
  auto integrityBytes = core::serializeCanonicalJson(withoutIntegrity.value());
  if (!integrityBytes.ok()) {
    return integrityBytes.error();
  }
  evidence.integrityHash = core::sha256(integrityBytes.value());
  auto complete = evidenceJson(evidence, true);
  if (!complete.ok()) {
    return complete.error();
  }
  auto serialized = core::serializeCanonicalJson(complete.value());
  if (!serialized.ok()) {
    return serialized.error();
  }
  if (serialized.value().size() > maximumEvidenceBytes) {
    return invalidArgument("Verification evidence exceeds the 64 KiB limit");
  }
  return serialized;
}

core::Result<void> writeAtomic(const std::filesystem::path &path,
                               const std::string_view bytes,
                               const bool createOnly) {
  std::error_code directoryError;
  std::filesystem::create_directories(path.parent_path(), directoryError);
  if (directoryError) {
    return fileError("Unable to create verification directory",
                     directoryError.message());
  }
  auto temporary = path;
  temporary += ".tmp-" + randomIdentity();

#if defined(__APPLE__) || defined(__linux__)
  const auto temporaryName = temporary.string();
  const auto destinationName = path.string();
  const auto descriptor =
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): stage O_CREAT mode
      ::open(temporaryName.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) {
    return fileError("Unable to create verification staging file",
                     std::error_code{errno, std::generic_category()}.message());
  }
  std::size_t writtenBytes = 0;
  while (writtenBytes < bytes.size()) {
    const auto written = ::write(descriptor, bytes.data() + writtenBytes,
                                 bytes.size() - writtenBytes);
    if (written <= 0) {
      const auto savedError = written < 0 ? errno : EIO;
      static_cast<void>(::close(descriptor));
      static_cast<void>(::unlink(temporaryName.c_str()));
      return fileError(
          "Unable to write verification staging file",
          std::error_code{savedError, std::generic_category()}.message());
    }
    writtenBytes += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    const auto savedError = errno;
    static_cast<void>(::close(descriptor));
    static_cast<void>(::unlink(temporaryName.c_str()));
    return fileError(
        "Unable to flush verification staging file",
        std::error_code{savedError, std::generic_category()}.message());
  }
  if (::close(descriptor) != 0) {
    const auto savedError = errno;
    static_cast<void>(::unlink(temporaryName.c_str()));
    return fileError(
        "Unable to close verification staging file",
        std::error_code{savedError, std::generic_category()}.message());
  }
  if (createOnly) {
    if (::link(temporaryName.c_str(), destinationName.c_str()) != 0) {
      const auto savedError = errno;
      static_cast<void>(::unlink(temporaryName.c_str()));
      return core::makeError(
          savedError == EEXIST ? core::ErrorCode::resourceBusy
                               : core::ErrorCode::permissionDenied,
          "Unable to install verification evidence: " +
              std::string{std::error_code{savedError, std::generic_category()}
                              .message()});
    }
    static_cast<void>(::unlink(temporaryName.c_str()));
  } else if (::rename(temporaryName.c_str(), destinationName.c_str()) != 0) {
    const auto savedError = errno;
    static_cast<void>(::unlink(temporaryName.c_str()));
    return fileError(
        "Unable to replace verification evidence",
        std::error_code{savedError, std::generic_category()}.message());
  }
  const auto directoryDescriptor =
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): Directory fsync fd.
      ::open(path.parent_path().string().c_str(), O_RDONLY);
  if (directoryDescriptor >= 0) {
    static_cast<void>(::fsync(directoryDescriptor));
    static_cast<void>(::close(directoryDescriptor));
  }
  return {};
#else
  std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
  if (!output) {
    return fileError("Unable to create verification staging file",
                     "file could not be opened");
  }
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.flush();
  if (!output.good()) {
    output.close();
    std::filesystem::remove(temporary, directoryError);
    return fileError("Unable to flush verification staging file",
                     "write failed");
  }
  output.close();
  if (createOnly && std::filesystem::exists(path)) {
    std::filesystem::remove(temporary, directoryError);
    return core::makeError(core::ErrorCode::resourceBusy,
                           "Verification identity already exists");
  }
  std::filesystem::rename(temporary, path, directoryError);
  if (directoryError) {
    std::filesystem::remove(temporary, directoryError);
    return fileError("Unable to install verification evidence",
                     directoryError.message());
  }
  return {};
#endif
}

core::Result<std::string> readFile(const std::filesystem::path &path) {
  std::error_code statusError;
  const auto status = std::filesystem::symlink_status(path, statusError);
  if (statusError == std::errc::no_such_file_or_directory) {
    return core::makeError(core::ErrorCode::notFound,
                           "Verification evidence is unavailable");
  }
  if (statusError || !std::filesystem::is_regular_file(status)) {
    return schemaError("Verification evidence is not a regular file");
  }
  std::error_code sizeError;
  const auto size = std::filesystem::file_size(path, sizeError);
  if (sizeError || size > maximumEvidenceBytes) {
    return schemaError(
        "Verification evidence is unavailable or exceeds 64 KiB");
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return core::makeError(core::ErrorCode::notFound,
                           "Verification evidence is unavailable");
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!input || input.peek() != std::char_traits<char>::eof()) {
    return schemaError("Verification evidence changed while being read");
  }
  return bytes;
}

struct VerificationAttemptPosition final {
  std::size_t ordinal = 0;
  std::size_t count = 0;
};

core::Result<std::optional<std::string>>
readOptionalAttemptString(const nlohmann::json &attemptJsonValue,
                          const std::string_view key,
                          const std::size_t maxBytes) {
  const auto &value = attemptJsonValue[std::string{key}];
  if (value.is_null()) {
    return std::optional<std::string>{};
  }
  if (!value.is_string()) {
    return schemaError("Verification attempt string field is invalid");
  }
  auto stringValue = value.get<std::string>();
  if (stringValue.size() > maxBytes) {
    return schemaError("Verification attempt string exceeds its limit");
  }
  return std::optional<std::string>{std::move(stringValue)};
}

core::Result<void>
parseAttemptTerminalStatus(const std::optional<std::string> &statusText,
                           VerificationAttempt &attempt) {
  if (!statusText.has_value()) {
    return {};
  }
  const auto terminalStatus = parseRunStatus(statusText.value());
  if (!terminalStatus.has_value() ||
      terminalStatus.value() == RunStatus::running) {
    return schemaError("Verification attempt terminal status is invalid");
  }
  attempt.terminalStatus = terminalStatus;
  return {};
}

core::Result<std::optional<VerificationError>>
parseAttemptError(const nlohmann::json &error) {
  if (error.is_null()) {
    return std::optional<VerificationError>{};
  }
  if (!isObjectWithKeys(error, {"code", "message"}) ||
      !error["code"].is_string() || !error["message"].is_string()) {
    return schemaError("Verification attempt error is invalid");
  }
  const auto code = error["code"].get<std::string>();
  const auto message = error["message"].get<std::string>();
  if (code.empty() || code.size() > maximumErrorCodeBytes ||
      message.size() > maximumErrorMessageBytes) {
    return schemaError("Verification attempt error exceeds its limit");
  }
  return std::optional<VerificationError>{
      VerificationError{.code = code, .message = message}};
}

core::Result<void>
validateAttemptResultReferences(const VerificationAttempt &attempt) {
  if (attempt.resultId.has_value() &&
      !validIdentity(attempt.resultId.value())) {
    return schemaError("Verification attempt Result ID is invalid");
  }
  if (attempt.canonicalResultHash.has_value() &&
      !validHex(attempt.canonicalResultHash.value(), 64)) {
    return schemaError("Verification attempt Result hash is invalid");
  }
  return {};
}

core::Result<void>
validateParsedAttemptState(const VerificationAttempt &attempt,
                           const VerificationAttemptPosition position) {
  if (attempt.terminalStatus.has_value() && !attempt.resultId.has_value()) {
    return schemaError("Terminal verification attempt has no Result ID");
  }
  if (attempt.terminalStatus == RunStatus::completed &&
      !attempt.canonicalResultHash.has_value()) {
    return schemaError("Completed verification attempt has no Result hash");
  }
  if (position.ordinal < position.count &&
      !attempt.terminalStatus.has_value()) {
    return schemaError("Only the final verification attempt may be active");
  }
  return {};
}

core::Result<VerificationAttempt>
parseVerificationAttempt(const nlohmann::json &attemptJsonValue,
                         const VerificationAttemptPosition position) {
  if (!isObjectWithKeys(attemptJsonValue,
                        {"attemptOrdinal", "canonicalResultHash", "error",
                         "resultId", "terminalStatus"}) ||
      !attemptJsonValue["attemptOrdinal"].is_number_unsigned() ||
      attemptJsonValue["attemptOrdinal"].get<std::size_t>() !=
          position.ordinal) {
    return schemaError("Verification attempt metadata is invalid");
  }
  VerificationAttempt attempt;
  attempt.ordinal = position.ordinal;
  auto resultId = readOptionalAttemptString(attemptJsonValue, "resultId", 32);
  auto resultHash =
      readOptionalAttemptString(attemptJsonValue, "canonicalResultHash", 64);
  auto status =
      readOptionalAttemptString(attemptJsonValue, "terminalStatus", 32);
  if (!resultId.ok() || !resultHash.ok() || !status.ok()) {
    return schemaError("Verification attempt fields are invalid");
  }
  attempt.resultId = std::move(resultId).value();
  attempt.canonicalResultHash = std::move(resultHash).value();
  auto referencesValid = validateAttemptResultReferences(attempt);
  if (!referencesValid.ok()) {
    return referencesValid.error();
  }
  auto terminalStatus =
      parseAttemptTerminalStatus(std::move(status).value(), attempt);
  if (!terminalStatus.ok()) {
    return terminalStatus.error();
  }
  auto error = parseAttemptError(attemptJsonValue["error"]);
  if (!error.ok()) {
    return error.error();
  }
  attempt.error = std::move(error).value();
  auto stateValid = validateParsedAttemptState(attempt, position);
  if (!stateValid.ok()) {
    return stateValid.error();
  }
  return attempt;
}

core::Result<void>
validateVerificationEvidenceState(const VerificationEvidence &evidence) {
  if (!evidence.attempts.empty()) {
    const auto &last = evidence.attempts.back();
    if (evidence.status == VerificationStatus::running &&
        ((evidence.attempts.size() >= requiredAttemptCount &&
          last.terminalStatus.has_value()) ||
         last.error.has_value())) {
      return schemaError("Running verification has a terminal attempt state");
    }
    if (last.terminalStatus.has_value() &&
        last.terminalStatus.value() != RunStatus::completed &&
        evidence.status == VerificationStatus::running) {
      return schemaError("Running verification has a failed attempt");
    }
  }
  if (evidence.status == VerificationStatus::verified) {
    const auto firstHash = evidence.attempts.empty()
                               ? std::optional<std::string>{}
                               : evidence.attempts.front().canonicalResultHash;
    const auto allCompleted =
        std::ranges::all_of(evidence.attempts, [](const auto &attempt) {
          return attempt.terminalStatus == RunStatus::completed &&
                 attempt.canonicalResultHash.has_value() &&
                 !attempt.error.has_value();
        });
    const auto hashesMatch = std::ranges::all_of(
        evidence.attempts, [firstHash](const auto &attempt) {
          return attempt.canonicalResultHash == firstHash;
        });
    if (evidence.attempts.size() != requiredAttemptCount || !allCompleted ||
        !hashesMatch) {
      return schemaError(
          "Verified evidence does not contain three matching runs");
    }
  }
  return {};
}

core::Result<VerificationEvidence>
parseEvidence(const std::filesystem::path &path,
              const std::string_view expectedId) {
  auto bytes = readFile(path);
  if (!bytes.ok()) {
    return bytes.error();
  }
  auto parsed = core::parseCanonicalJson(bytes.value());
  if (!parsed.ok() ||
      !isObjectWithKeys(parsed.value(),
                        {"attempts", "identity", "identityHash",
                         "integrityHash", "progressOrdinal", "schemaVersion",
                         "state", "verificationId"})) {
    return schemaError("Verification evidence schema is invalid");
  }
  auto canonicalBytes = core::serializeCanonicalJson(parsed.value());
  if (!canonicalBytes.ok() || canonicalBytes.value() != bytes.value()) {
    return schemaError("Verification evidence is not canonical JSON");
  }
  const auto &json = parsed.value();
  if (!json["schemaVersion"].is_number_integer() ||
      json["schemaVersion"].get<std::int64_t>() != 1 ||
      !json["verificationId"].is_string() ||
      json["verificationId"].get<std::string>() != expectedId ||
      !validIdentity(expectedId) || !json["identity"].is_object() ||
      !json["identityHash"].is_string() || !json["integrityHash"].is_string() ||
      !json["attempts"].is_array() ||
      !json["progressOrdinal"].is_number_unsigned() ||
      !json["state"].is_string()) {
    return schemaError("Verification evidence metadata is invalid");
  }
  auto identityJson = core::serializeCanonicalJson(json["identity"]);
  const auto identityHash = json["identityHash"].get<std::string>();
  const auto integrityHash = json["integrityHash"].get<std::string>();
  if (!identityJson.ok() || !validHex(identityHash, 64) ||
      core::sha256(identityJson.value()) != identityHash ||
      !validHex(integrityHash, 64)) {
    return schemaError("Verification evidence hashes are invalid");
  }
  auto integrityDocument = json;
  integrityDocument.erase("integrityHash");
  auto integrityBytes = core::serializeCanonicalJson(integrityDocument);
  if (!integrityBytes.ok() ||
      core::sha256(integrityBytes.value()) != integrityHash) {
    return schemaError("Verification evidence integrity hash is invalid");
  }
  const auto state = parseVerificationStatus(json["state"].get<std::string>());
  if (!state) {
    return schemaError("Verification evidence state is invalid");
  }
  VerificationEvidence result{
      .verificationId = std::string{expectedId},
      .identity = {.canonicalJson = std::move(identityJson).value(),
                   .sha256 = identityHash},
      .status = state.value(),
      .progressOrdinal = json["progressOrdinal"].get<std::size_t>(),
      .attempts = {},
      .integrityHash = integrityHash,
  };
  if (result.progressOrdinal > requiredAttemptCount ||
      result.progressOrdinal != json["attempts"].size()) {
    return schemaError("Verification progress ordinal is invalid");
  }
  for (std::size_t ordinal = 1; ordinal <= json["attempts"].size(); ++ordinal) {
    const auto index = ordinal - 1;
    auto attempt = parseVerificationAttempt(
        json["attempts"][index],
        {.ordinal = ordinal, .count = json["attempts"].size()});
    if (!attempt.ok()) {
      return attempt.error();
    }
    result.attempts.push_back(std::move(attempt).value());
  }
  const auto stateValidation = validateVerificationEvidenceState(result);
  if (!stateValidation.ok()) {
    return stateValidation.error();
  }
  return result;
}

core::Result<void> persistEvidence(const std::filesystem::path &directory,
                                   VerificationEvidence &evidence,
                                   const bool createOnly) {
  auto bytes = evidenceBytes(evidence);
  if (!bytes.ok()) {
    return bytes.error();
  }
  return writeAtomic(directory / (evidence.verificationId + ".bteverify"),
                     bytes.value(), createOnly);
}

core::Result<VerificationEvidence>
loadEvidence(const std::filesystem::path &directory,
             const std::string_view verificationId) {
  if (!validIdentity(verificationId)) {
    return invalidArgument("Verification ID is invalid");
  }
  return parseEvidence(directory / (std::string{verificationId} + ".bteverify"),
                       verificationId);
}

bool attemptIsActive(const VerificationAttempt &attempt) {
  return !attempt.terminalStatus.has_value() && !attempt.error.has_value();
}

core::Result<VerificationAttempt *>
activeAttempt(VerificationEvidence &evidence) {
  if (evidence.status != VerificationStatus::running ||
      evidence.attempts.empty() || !attemptIsActive(evidence.attempts.back())) {
    return invalidArgument("Verification has no active attempt");
  }
  return &evidence.attempts.back();
}

core::Result<std::vector<std::filesystem::path>>
verificationPaths(const std::filesystem::path &directory) {
  std::vector<std::filesystem::path> paths;
  std::error_code error;
  std::filesystem::directory_iterator iterator{directory, error};
  if (error) {
    return fileError("Unable to list verification evidence", error.message());
  }
  const std::filesystem::directory_iterator end;
  for (; iterator != end; iterator.increment(error)) {
    const auto entryStatus = iterator->symlink_status(error);
    if (error) {
      return fileError("Unable to inspect verification evidence",
                       error.message());
    }
    if (!std::filesystem::is_regular_file(entryStatus) ||
        iterator->path().extension() != ".bteverify") {
      continue;
    }
    const auto verificationId = iterator->path().stem().string();
    if (validIdentity(verificationId)) {
      paths.push_back(iterator->path());
    }
  }
  if (error) {
    return fileError("Unable to list verification evidence", error.message());
  }
  std::ranges::sort(paths);
  return paths;
}

} // namespace

struct VerificationStore::Impl final {
  std::filesystem::path directory;
};

VerificationStore::VerificationStore(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

VerificationStore::~VerificationStore() = default;

core::Result<std::unique_ptr<VerificationStore>>
VerificationStore::open(const std::filesystem::path &resultStoreRoot) {
  if (resultStoreRoot.empty()) {
    return invalidArgument("Result store root is required");
  }
  const auto directory = resultStoreRoot / "Verifications";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error || !std::filesystem::is_directory(directory)) {
    return fileError("Unable to prepare verification directory",
                     error ? error.message() : "path is not a directory");
  }
  auto impl = std::make_unique<Impl>();
  impl->directory = directory;
  return std::make_unique<VerificationStore>(std::move(impl));
}

core::Result<VerificationEvidence>
VerificationStore::create(const RunDescriptor &descriptor) {
  auto identity = functionalInputIdentity(descriptor);
  if (!identity.ok()) {
    return identity.error();
  }
  VerificationEvidence capacityProbe{
      .verificationId = std::string(32, '0'),
      .identity = identity.value(),
      .status = VerificationStatus::failed,
      .progressOrdinal = requiredAttemptCount,
      .attempts = {},
      .integrityHash = {},
  };
  capacityProbe.attempts.reserve(requiredAttemptCount);
  for (std::size_t index = 0; index < requiredAttemptCount; ++index) {
    capacityProbe.attempts.push_back(VerificationAttempt{
        .ordinal = index + 1,
        .resultId = std::string(32, '1'),
        .terminalStatus = RunStatus::failed,
        .canonicalResultHash = std::string(64, 'a'),
        .error =
            VerificationError{.code = std::string(maximumErrorCodeBytes, 'e'),
                              .message =
                                  std::string(maximumErrorMessageBytes, 'x')},
    });
  }
  auto capacity = evidenceBytes(capacityProbe);
  if (!capacity.ok()) {
    return capacity.error();
  }
  const std::scoped_lock lock{storeMutex()};
  auto fileLock = VerificationFileLock::acquire(impl_->directory);
  if (!fileLock.ok()) {
    return fileLock.error();
  }
  for (int attempt = 0; attempt < 8; ++attempt) {
    VerificationEvidence evidence{
        .verificationId = randomIdentity(),
        .identity = identity.value(),
        .status = VerificationStatus::running,
        .progressOrdinal = 0,
        .attempts = {},
        .integrityHash = {},
    };
    auto persisted = persistEvidence(impl_->directory, evidence, true);
    if (persisted.ok()) {
      return evidence;
    }
    if (persisted.error().code != core::ErrorCode::resourceBusy) {
      return persisted.error();
    }
  }
  return core::makeError(core::ErrorCode::resourceBusy,
                         "Unable to allocate a unique verification ID");
}

core::Result<VerificationEvidence>
VerificationStore::load(const std::string &verificationId) const {
  const std::scoped_lock lock{storeMutex()};
  auto fileLock = VerificationFileLock::acquire(impl_->directory);
  if (!fileLock.ok()) {
    return fileLock.error();
  }
  return loadEvidence(impl_->directory, verificationId);
}

core::Result<std::vector<VerificationListEntry>>
VerificationStore::list() const {
  const std::scoped_lock lock{storeMutex()};
  auto fileLock = VerificationFileLock::acquire(impl_->directory);
  if (!fileLock.ok()) {
    return fileLock.error();
  }
  auto paths = verificationPaths(impl_->directory);
  if (!paths.ok()) {
    return paths.error();
  }
  std::vector<VerificationListEntry> entries;
  for (const auto &path : paths.value()) {
    auto evidence = parseEvidence(path, path.stem().string());
    if (evidence.ok()) {
      entries.push_back({.verificationId = path.stem().string(),
                         .evidence = std::move(evidence).value(),
                         .diagnostic = {}});
    } else {
      entries.push_back({.verificationId = path.stem().string(),
                         .evidence = std::nullopt,
                         .diagnostic = evidence.error().message});
    }
  }
  return entries;
}

core::Result<VerificationEvidence>
VerificationStore::beginAttempt(const std::string &verificationId) {
  const std::scoped_lock lock{storeMutex()};
  auto fileLock = VerificationFileLock::acquire(impl_->directory);
  if (!fileLock.ok()) {
    return fileLock.error();
  }
  auto evidence = loadEvidence(impl_->directory, verificationId);
  if (!evidence.ok()) {
    return evidence.error();
  }
  if (evidence.value().status != VerificationStatus::running ||
      evidence.value().attempts.size() >= requiredAttemptCount ||
      (!evidence.value().attempts.empty() &&
       attemptIsActive(evidence.value().attempts.back()))) {
    return invalidArgument("Verification cannot start another attempt");
  }
  auto updated = std::move(evidence).value();
  updated.attempts.push_back(VerificationAttempt{
      .ordinal = updated.attempts.size() + 1,
      .resultId = std::nullopt,
      .terminalStatus = std::nullopt,
      .canonicalResultHash = std::nullopt,
      .error = std::nullopt,
  });
  updated.progressOrdinal = updated.attempts.size();
  auto persisted = persistEvidence(impl_->directory, updated, false);
  if (!persisted.ok()) {
    return persisted.error();
  }
  return updated;
}

core::Result<VerificationEvidence>
VerificationStore::attachResultId(const std::string &verificationId,
                                  const std::string &resultId) {
  if (!validIdentity(resultId)) {
    return invalidArgument("Attempt Result ID is invalid");
  }
  const std::scoped_lock lock{storeMutex()};
  auto fileLock = VerificationFileLock::acquire(impl_->directory);
  if (!fileLock.ok()) {
    return fileLock.error();
  }
  auto evidence = loadEvidence(impl_->directory, verificationId);
  if (!evidence.ok()) {
    return evidence.error();
  }
  auto updated = std::move(evidence).value();
  auto active = activeAttempt(updated);
  if (!active.ok()) {
    return active.error();
  }
  if (active.value()->resultId.value_or(resultId) != resultId) {
    return invalidArgument("Attempt Result ID is already assigned");
  }
  active.value()->resultId = resultId;
  auto persisted = persistEvidence(impl_->directory, updated, false);
  if (!persisted.ok()) {
    return persisted.error();
  }
  return updated;
}

core::Result<VerificationEvidence> VerificationStore::completeAttempt(
    const std::string &verificationId, const RunDescriptor &attemptDescriptor,
    const FinalizedResult &result, const RunStatus terminalStatus) {
  if (!validIdentity(result.resultId) ||
      !validHex(result.canonicalResultHash, 64) ||
      terminalStatus == RunStatus::running ||
      runStatusName(terminalStatus).empty()) {
    return invalidArgument("Attempt Result is not terminal and validated");
  }
  const std::scoped_lock lock{storeMutex()};
  auto fileLock = VerificationFileLock::acquire(impl_->directory);
  if (!fileLock.ok()) {
    return fileLock.error();
  }
  auto evidence = loadEvidence(impl_->directory, verificationId);
  if (!evidence.ok()) {
    return evidence.error();
  }
  auto updated = std::move(evidence).value();
  auto active = activeAttempt(updated);
  if (!active.ok()) {
    return active.error();
  }
  auto &attempt = *active.value();
  if (attempt.resultId.value_or(result.resultId) != result.resultId) {
    attempt.error = VerificationError{
        .code = "attemptResultIdMismatch",
        .message = "The finalized Result ID differs from the active attempt"};
    attempt.terminalStatus = terminalStatus;
    attempt.canonicalResultHash = result.canonicalResultHash;
    updated.status = VerificationStatus::mismatch;
  } else {
    attempt.resultId = result.resultId;
    attempt.terminalStatus = terminalStatus;
    attempt.canonicalResultHash = result.canonicalResultHash;
    auto identity = functionalInputIdentity(attemptDescriptor);
    if (!identity.ok() || identity.value().sha256 != updated.identity.sha256 ||
        identity.value().canonicalJson != updated.identity.canonicalJson) {
      attempt.error = VerificationError{
          .code = "functionalInputMismatch",
          .message =
              "Attempt functional inputs differ from the frozen identity"};
      updated.status = VerificationStatus::mismatch;
    } else if (terminalStatus == RunStatus::canceled) {
      updated.status = VerificationStatus::canceled;
    } else if (terminalStatus == RunStatus::interrupted) {
      updated.status = VerificationStatus::interrupted;
    } else if (terminalStatus != RunStatus::completed) {
      updated.status = VerificationStatus::failed;
    } else if (updated.attempts.size() > 1 &&
               updated.attempts.front().canonicalResultHash !=
                   attempt.canonicalResultHash) {
      updated.status = VerificationStatus::mismatch;
    } else if (updated.attempts.size() == requiredAttemptCount) {
      updated.status = VerificationStatus::verified;
    }
  }
  auto persisted = persistEvidence(impl_->directory, updated, false);
  if (!persisted.ok()) {
    return persisted.error();
  }
  return updated;
}

core::Result<VerificationEvidence>
VerificationStore::failAttempt(const std::string_view verificationId,
                               std::string errorCode, std::string errorMessage,
                               std::optional<std::string> resultId) {
  if (errorCode.empty() || errorCode.size() > maximumErrorCodeBytes ||
      errorMessage.size() > maximumErrorMessageBytes ||
      (resultId.has_value() && !validIdentity(*resultId))) {
    return invalidArgument("Verification failure details exceed their limits");
  }
  const std::scoped_lock lock{storeMutex()};
  auto fileLock = VerificationFileLock::acquire(impl_->directory);
  if (!fileLock.ok()) {
    return fileLock.error();
  }
  auto evidence = loadEvidence(impl_->directory, verificationId);
  if (!evidence.ok()) {
    return evidence.error();
  }
  auto updated = std::move(evidence).value();
  auto active = activeAttempt(updated);
  if (!active.ok()) {
    return active.error();
  }
  auto &attempt = *active.value();
  if (attempt.resultId.has_value() && resultId.has_value() &&
      attempt.resultId.value_or("") != resultId.value_or("")) {
    return invalidArgument("Failure Result ID differs from the active attempt");
  }
  if (!attempt.resultId.has_value()) {
    attempt.resultId = std::move(resultId);
  }
  attempt.error = VerificationError{.code = std::move(errorCode),
                                    .message = std::move(errorMessage)};
  updated.status = VerificationStatus::failed;
  auto persisted = persistEvidence(impl_->directory, updated, false);
  if (!persisted.ok()) {
    return persisted.error();
  }
  return updated;
}

core::Result<VerificationEvidence>
VerificationStore::cancelAttempt(const std::string_view verificationId,
                                 std::string errorMessage,
                                 std::optional<std::string> resultId) {
  if (errorMessage.size() > maximumErrorMessageBytes ||
      (resultId.has_value() && !validIdentity(*resultId))) {
    return invalidArgument(
        "Verification cancellation details exceed their limits");
  }
  const std::scoped_lock lock{storeMutex()};
  auto fileLock = VerificationFileLock::acquire(impl_->directory);
  if (!fileLock.ok()) {
    return fileLock.error();
  }
  auto evidence = loadEvidence(impl_->directory, verificationId);
  if (!evidence.ok()) {
    return evidence.error();
  }
  auto updated = std::move(evidence).value();
  auto active = activeAttempt(updated);
  if (!active.ok()) {
    return active.error();
  }
  auto &attempt = *active.value();
  if (attempt.resultId.has_value() && resultId.has_value() &&
      attempt.resultId.value_or("") != resultId.value_or("")) {
    return invalidArgument(
        "Canceled Result ID differs from the active attempt");
  }
  if (!attempt.resultId.has_value()) {
    attempt.resultId = std::move(resultId);
  }
  attempt.error = VerificationError{
      .code = "canceled",
      .message = errorMessage.empty() ? "Verification attempt was canceled"
                                      : std::move(errorMessage),
  };
  updated.status = VerificationStatus::canceled;
  auto persisted = persistEvidence(impl_->directory, updated, false);
  if (!persisted.ok()) {
    return persisted.error();
  }
  return updated;
}

core::Result<VerificationEvidence>
VerificationStore::cancel(const std::string &verificationId) {
  const std::scoped_lock lock{storeMutex()};
  auto fileLock = VerificationFileLock::acquire(impl_->directory);
  if (!fileLock.ok()) {
    return fileLock.error();
  }
  auto evidence = loadEvidence(impl_->directory, verificationId);
  if (!evidence.ok()) {
    return evidence.error();
  }
  auto updated = std::move(evidence).value();
  if (updated.status != VerificationStatus::running ||
      (!updated.attempts.empty() && attemptIsActive(updated.attempts.back()))) {
    return invalidArgument(
        "Cancel the active Backtest before canceling verification");
  }
  updated.status = VerificationStatus::canceled;
  auto persisted = persistEvidence(impl_->directory, updated, false);
  if (!persisted.ok()) {
    return persisted.error();
  }
  return updated;
}

core::Result<std::vector<VerificationEvidence>>
VerificationStore::recoverInterrupted() {
  const std::scoped_lock lock{storeMutex()};
  auto fileLock = VerificationFileLock::acquire(impl_->directory);
  if (!fileLock.ok()) {
    return fileLock.error();
  }
  auto paths = verificationPaths(impl_->directory);
  if (!paths.ok()) {
    return paths.error();
  }
  std::vector<VerificationEvidence> recovered;
  for (const auto &path : paths.value()) {
    auto evidence = parseEvidence(path, path.stem().string());
    if (!evidence.ok() ||
        evidence.value().status != VerificationStatus::running) {
      continue;
    }
    auto updated = std::move(evidence).value();
    if (!updated.attempts.empty() && attemptIsActive(updated.attempts.back())) {
      auto &attempt = updated.attempts.back();
      if (attempt.resultId.has_value()) {
        attempt.terminalStatus = RunStatus::interrupted;
      }
      attempt.error = VerificationError{
          .code = "applicationInterrupted",
          .message = "Application stopped before the verification completed"};
    }
    updated.status = VerificationStatus::interrupted;
    auto persisted = persistEvidence(impl_->directory, updated, false);
    if (!persisted.ok()) {
      return persisted.error();
    }
    recovered.push_back(std::move(updated));
  }
  return recovered;
}

core::Result<bool>
VerificationStore::hasVerifiedEvidence(const RunDescriptor &descriptor) const {
  auto identity = functionalInputIdentity(descriptor);
  if (!identity.ok()) {
    return identity.error();
  }
  const std::scoped_lock lock{storeMutex()};
  auto fileLock = VerificationFileLock::acquire(impl_->directory);
  if (!fileLock.ok()) {
    return fileLock.error();
  }
  auto paths = verificationPaths(impl_->directory);
  if (!paths.ok()) {
    return paths.error();
  }
  for (const auto &path : paths.value()) {
    auto evidence = parseEvidence(path, path.stem().string());
    if (evidence.ok() &&
        evidence.value().status == VerificationStatus::verified &&
        evidence.value().identity == identity.value()) {
      return true;
    }
  }
  return false;
}

core::Result<bool>
VerificationStore::hasVerifiedResult(const RunDescriptor &descriptor,
                                     const FinalizedResult &result) const {
  if (!validIdentity(result.resultId) ||
      !validHex(result.canonicalResultHash, 64)) {
    return invalidArgument("Verified Result identity is invalid");
  }
  auto identity = functionalInputIdentity(descriptor);
  if (!identity.ok()) {
    return identity.error();
  }
  const std::scoped_lock lock{storeMutex()};
  auto fileLock = VerificationFileLock::acquire(impl_->directory);
  if (!fileLock.ok()) {
    return fileLock.error();
  }
  auto paths = verificationPaths(impl_->directory);
  if (!paths.ok()) {
    return paths.error();
  }
  for (const auto &path : paths.value()) {
    auto evidence = parseEvidence(path, path.stem().string());
    if (!evidence.ok() ||
        evidence.value().status != VerificationStatus::verified ||
        evidence.value().identity != identity.value()) {
      continue;
    }
    if (std::ranges::any_of(evidence.value().attempts,
                            [&result](const VerificationAttempt &attempt) {
                              return attempt.canonicalResultHash ==
                                     result.canonicalResultHash;
                            })) {
      return true;
    }
  }
  return false;
}

} // namespace bte::results
