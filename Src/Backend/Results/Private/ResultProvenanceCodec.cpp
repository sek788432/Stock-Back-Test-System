#include "ResultProvenanceCodec.h"

#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "Bte/Core/CanonicalJson.h"

namespace bte::results::detail {

using core::parseCanonicalJson;
using core::serializeCanonicalJson;

std::string_view strategyModeName(const StrategyMode mode) {
  switch (mode) {
  case StrategyMode::starter:
    return "starter";
  case StrategyMode::selectableConditions:
    return "selectable-conditions";
  case StrategyMode::python:
    return "python";
  case StrategyMode::legacyUnrecorded:
    return "legacy-unrecorded";
  }
  return {};
}

namespace {

std::optional<StrategyMode> parseStrategyMode(const std::string_view value) {
  if (value == "starter") {
    return StrategyMode::starter;
  }
  if (value == "selectable-conditions") {
    return StrategyMode::selectableConditions;
  }
  if (value == "python") {
    return StrategyMode::python;
  }
  if (value == "legacy-unrecorded") {
    return StrategyMode::legacyUnrecorded;
  }
  return std::nullopt;
}

} // namespace

std::optional<std::string_view>
availabilityName(const ProvenanceAvailabilityState state) {
  switch (state) {
  case ProvenanceAvailabilityState::recorded:
    return "recorded";
  case ProvenanceAvailabilityState::legacyUnrecorded:
    return "legacy-unrecorded";
  }
  return std::nullopt;
}

core::Result<std::string>
serializeStoredProvenance(const ResultProvenance &provenance) {
  nlohmann::json availability = nlohmann::json::object();
  for (const auto &[key, state] : provenance.provenanceAvailability) {
    const auto value = availabilityName(state);
    if (!value.has_value()) {
      return core::makeError(core::ErrorCode::invalidArgument,
                             "Provenance availability state is invalid");
    }
    availability[key] = *value;
  }

  nlohmann::json runtime = nullptr;
  if (provenance.runtime.has_value()) {
    const auto &profile = *provenance.runtime;
    nlohmann::json packages = nlohmann::json::array();
    for (const auto &package : profile.packages) {
      packages.push_back({{"artifactHash", package.artifactHash},
                          {"name", package.name},
                          {"version", package.version}});
    }
    runtime = {{"abi", profile.pythonAbi},
               {"manifest", profile.canonicalManifestJson},
               {"manifestHash", profile.manifestHash},
               {"packages", std::move(packages)},
               {"profileId", profile.profileId},
               {"pythonVersion", profile.pythonVersion},
               {"stockbtSourceHash", profile.stockbtSourceHash}};
  }
  const auto optionalText =
      [](const std::optional<std::string> &value) -> nlohmann::json {
    return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
  };
  const auto optionalIntegerText =
      [](const std::optional<std::int64_t> value) -> nlohmann::json {
    return value.has_value() ? nlohmann::json(std::to_string(*value))
                             : nlohmann::json(nullptr);
  };
  const nlohmann::json value{
      {"apiVersion", provenance.apiVersion},
      {"canonicalParametersJson", provenance.canonicalParametersJson},
      {"canonicalPlanJson", provenance.canonicalPlanJson},
      {"costProfile", optionalText(provenance.costProfile)},
      {"engineBuild", optionalText(provenance.engineBuild)},
      {"provenanceAvailability", std::move(availability)},
      {"runtime", std::move(runtime)},
      {"seed", optionalIntegerText(provenance.seed)},
      {"slippageRatePartsPerBillion",
       optionalIntegerText(provenance.slippageRatePartsPerBillion)},
      {"commissionRatePartsPerBillion",
       optionalIntegerText(provenance.commissionRatePartsPerBillion)},
      {"sourceHash", provenance.sourceHash},
      {"sourceUtf8", provenance.sourceUtf8},
      {"strategyMode", std::string{strategyModeName(provenance.strategyMode)}},
      {"templateVersion", provenance.templateVersion},
      {"timeframe", provenance.timeframe},
      {"planHash", provenance.planHash},
  };
  return serializeCanonicalJson(value);
}

namespace {

core::Error invalidStoredProvenance() {
  return core::makeError(core::ErrorCode::schemaMismatch,
                         "Schema-3 Result provenance is malformed");
} // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.

std::optional<std::string> storedStringField(const nlohmann::json &value,
                                             const std::string_view key) {
  const auto entry = value.find(std::string{key});
  if (entry == value.end() || !entry->is_string()) {
    return std::nullopt;
  }
  return entry->get<std::string>();
}

bool readOptionalStoredString(const nlohmann::json &value,
                              const std::string_view key,
                              std::optional<std::string> &output) {
  const auto entry = value.find(std::string{key});
  if (entry == value.end()) {
    return false;
  }
  if (entry->is_null()) {
    output.reset();
    return true;
  }
  if (!entry->is_string()) {
    return false;
  }
  output = entry->get<std::string>();
  return true;
}

std::optional<std::int64_t> parseStoredInteger(const std::string &text) {
  std::int64_t result = 0;
  const auto parsedInteger =
      std::from_chars(text.data(), text.data() + text.size(), result, 10);
  if (parsedInteger.ec != std::errc{} ||
      parsedInteger.ptr != text.data() + text.size() ||
      std::to_string(result) != text) {
    return std::nullopt;
  }
  return result;
}

std::optional<std::optional<std::int64_t>>
parseOptionalStoredInteger(const std::optional<std::string> &text) {
  if (!text.has_value()) {
    return std::optional<std::int64_t>{};
  }
  const auto value = parseStoredInteger(*text);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return std::optional<std::optional<std::int64_t>>{std::in_place, *value};
}

core::Result<ResultProvenance>
parseStoredProvenanceScalars(const nlohmann::json &value) {
  const auto apiVersion = storedStringField(value, "apiVersion");
  const auto parameters = storedStringField(value, "canonicalParametersJson");
  const auto plan = storedStringField(value, "canonicalPlanJson");
  const auto planHash = storedStringField(value, "planHash");
  const auto sourceHash = storedStringField(value, "sourceHash");
  const auto source = storedStringField(value, "sourceUtf8");
  const auto modeText = storedStringField(value, "strategyMode");
  const auto templateVersion = storedStringField(value, "templateVersion");
  const auto timeframe = storedStringField(value, "timeframe");
  std::optional<std::string> costProfile;
  std::optional<std::string> engineBuild;
  std::optional<std::string> commission;
  std::optional<std::string> seed;
  std::optional<std::string> slippage;
  if (!apiVersion || !parameters || !plan || !planHash || !sourceHash ||
      !source || !modeText || !templateVersion || !timeframe ||
      !readOptionalStoredString(value, "costProfile", costProfile) ||
      !readOptionalStoredString(value, "engineBuild", engineBuild) ||
      !readOptionalStoredString(value, "commissionRatePartsPerBillion",
                                commission) ||
      !readOptionalStoredString(value, "seed", seed) ||
      !readOptionalStoredString(value, "slippageRatePartsPerBillion",
                                slippage)) {
    return invalidStoredProvenance();
  }
  const auto mode = parseStrategyMode(*modeText);
  if (!mode.has_value()) {
    return invalidStoredProvenance();
  }
  const auto commissionValue = parseOptionalStoredInteger(commission);
  const auto seedValue = parseOptionalStoredInteger(seed);
  const auto slippageValue = parseOptionalStoredInteger(slippage);
  if (!commissionValue || !seedValue || !slippageValue) {
    return invalidStoredProvenance();
  }
  return ResultProvenance{
      .engineBuild = std::move(engineBuild),
      .timeframe = *timeframe,
      .costProfile = std::move(costProfile),
      .commissionRatePartsPerBillion = commissionValue.value(),
      .slippageRatePartsPerBillion = slippageValue.value(),
      .seed = seedValue.value(),
      .strategyMode = *mode,
      .apiVersion = *apiVersion,
      .templateVersion = *templateVersion,
      .canonicalParametersJson = *parameters,
      .sourceUtf8 = *source,
      .sourceHash = *sourceHash,
      .canonicalPlanJson = *plan,
      .planHash = *planHash,
      .runtime = std::nullopt,
      .provenanceAvailability = {},
  };
}

core::Result<std::map<std::string, ProvenanceAvailabilityState>>
parseStoredProvenanceAvailability(const nlohmann::json &value) {
  const auto availability = value.find("provenanceAvailability");
  if (availability == value.end() || !availability->is_object()) {
    return invalidStoredProvenance();
  }
  std::map<std::string, ProvenanceAvailabilityState> result;
  for (auto entry = availability->begin(); entry != availability->end();
       ++entry) {
    if (!entry.value().is_string()) {
      return invalidStoredProvenance();
    }
    const auto state = entry.value().get_ref<const std::string &>();
    if (state == "recorded") {
      result.emplace(entry.key(), ProvenanceAvailabilityState::recorded);
    } else if (state == "legacy-unrecorded") {
      result.emplace(entry.key(),
                     ProvenanceAvailabilityState::legacyUnrecorded);
    } else {
      return invalidStoredProvenance();
    }
  }
  return result;
}

core::Result<std::vector<RuntimePackageIdentity>>
parseStoredRuntimePackages(const nlohmann::json &packages) {
  static const std::set<std::string> packageKeys{"artifactHash", "name",
                                                 "version"};
  std::vector<RuntimePackageIdentity> result;
  for (const auto &package : packages) {
    if (!package.is_object() || package.size() != packageKeys.size() ||
        !std::ranges::all_of(packageKeys, [&package](const auto &key) {
          return package.contains(key);
        })) {
      return invalidStoredProvenance();
    }
    const auto name = package.find("name");
    const auto version = package.find("version");
    const auto hash = package.find("artifactHash");
    if (name == package.end() || !name->is_string() ||
        version == package.end() || !version->is_string() ||
        hash == package.end() || !hash->is_string()) {
      return invalidStoredProvenance();
    }
    result.push_back({.name = name->get<std::string>(),
                      .version = version->get<std::string>(),
                      .artifactHash = hash->get<std::string>()});
  }
  return result;
}

core::Result<std::optional<RuntimeProfileProvenance>>
parseStoredRuntime(const nlohmann::json &value) {
  const auto runtime = value.find("runtime");
  if (runtime == value.end()) {
    return invalidStoredProvenance();
  }
  if (runtime->is_null()) {
    return std::optional<RuntimeProfileProvenance>{};
  }
  static const std::set<std::string> runtimeKeys{
      "abi",       "manifest",      "manifestHash",     "packages",
      "profileId", "pythonVersion", "stockbtSourceHash"};
  if (!runtime->is_object() || runtime->size() != runtimeKeys.size() ||
      !std::ranges::all_of(runtimeKeys, [&runtime](const auto &key) {
        return runtime->contains(key);
      })) {
    return invalidStoredProvenance();
  }
  const auto abi = storedStringField(*runtime, "abi");
  const auto manifest = storedStringField(*runtime, "manifest");
  const auto manifestHash = storedStringField(*runtime, "manifestHash");
  const auto profileId = storedStringField(*runtime, "profileId");
  const auto pythonVersion = storedStringField(*runtime, "pythonVersion");
  const auto stockbtHash = storedStringField(*runtime, "stockbtSourceHash");
  const auto packages = runtime->find("packages");
  if (!abi || !manifest || !manifestHash || !profileId || !pythonVersion ||
      !stockbtHash || packages == runtime->end() || !packages->is_array()) {
    return invalidStoredProvenance();
  }
  auto parsedPackages = parseStoredRuntimePackages(*packages);
  if (!parsedPackages.ok()) {
    return parsedPackages.error();
  }
  RuntimeProfileProvenance profile{
      .profileId = *profileId,
      .canonicalManifestJson = *manifest,
      .manifestHash = *manifestHash,
      .pythonVersion = *pythonVersion,
      .pythonAbi = *abi,
      .packages = std::move(parsedPackages).value(),
      .stockbtSourceHash = *stockbtHash,
  };
  return std::optional<RuntimeProfileProvenance>{std::move(profile)};
}

} // namespace

core::Result<ResultProvenance>
parseStoredProvenance(const std::string_view bytes) {
  auto parsed = parseCanonicalJson(bytes);
  if (!parsed.ok() || !parsed.value().is_object()) {
    return invalidStoredProvenance();
  }
  const auto canonical = serializeCanonicalJson(parsed.value());
  if (!canonical.ok() || canonical.value() != bytes) {
    return invalidStoredProvenance();
  }
  static const std::set<std::string> keys{"apiVersion",
                                          "canonicalParametersJson",
                                          "canonicalPlanJson",
                                          "commissionRatePartsPerBillion",
                                          "costProfile",
                                          "engineBuild",
                                          "planHash",
                                          "provenanceAvailability",
                                          "runtime",
                                          "seed",
                                          "slippageRatePartsPerBillion",
                                          "sourceHash",
                                          "sourceUtf8",
                                          "strategyMode",
                                          "templateVersion",
                                          "timeframe"};
  const auto &value = parsed.value();
  if (value.size() != keys.size() ||
      !std::ranges::all_of(
          keys, [&value](const auto &key) { return value.contains(key); })) {
    return invalidStoredProvenance();
  }
  auto provenance = parseStoredProvenanceScalars(value);
  if (!provenance.ok()) {
    return provenance.error();
  }
  auto availability = parseStoredProvenanceAvailability(value);
  if (!availability.ok()) {
    return availability.error();
  }
  auto runtime = parseStoredRuntime(value);
  if (!runtime.ok()) {
    return runtime.error();
  }
  auto result = std::move(provenance).value();
  result.provenanceAvailability = std::move(availability).value();
  result.runtime = std::move(runtime).value();
  return result;
}

} // namespace bte::results::detail
