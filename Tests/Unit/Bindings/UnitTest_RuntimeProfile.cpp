#include "RuntimeProfile.h"
#include "StrategyProtocol.h"

#include <Bte/Core/Digest.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace {

using bte::bindings::python::discoverManagedRuntime;
using bte::bindings::python::serializeCanonicalJson;
using bte::bindings::python::verifyRuntimeProfile;

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::atomic_uint64_t nextId{0};
    const auto unique = nextId.fetch_add(1, std::memory_order_relaxed);
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("bte-python-profile-" + std::to_string(stamp) + "-" +
             std::to_string(unique));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void writeText(const std::filesystem::path &path, const std::string &text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.close();
  ASSERT_TRUE(output.good());
}

[[nodiscard]] nlohmann::json createComponent(const std::string_view name,
                                             const std::string_view version,
                                             const std::string_view digest,
                                             const std::string_view license,
                                             const std::string_view url) {
  return nlohmann::json{{"artifactSha256", digest},
                        {"licenses", nlohmann::json::array({license})},
                        {"name", name},
                        {"upstreamUrl", url},
                        {"version", version}};
}

nlohmann::json createFixtureManifest(const std::string &interpreterDigest) {
  auto components = nlohmann::json::array(
      {createComponent("CPython", "3.14.9",
                       "226d696642a8a53d03b4d5b255b1c319b1575a6d44bd723a95"
                       "3c9f507755e36a",
                       "PSF-2.0", "https://example.invalid/python.tar.gz"),
       createComponent("numpy", "2.5.4",
                       "ffa6ce09a1c6a08e9667dd9c97aa0b14184e8d18f2a14b78b2"
                       "a2328c9147f076",
                       "BSD-3-Clause", "https://example.invalid/numpy.whl"),
       createComponent("pandas", "3.0.6",
                       "ff51a4459ed036e93d1eb1bb5e6e7b28685d3cb6b7c12b91c0"
                       "5b31024e234729",
                       "BSD-3-Clause", "https://example.invalid/pandas.whl"),
       createComponent("python-dateutil", "2.9.0.post0",
                       "a8b2bc7bffae282281c8140a97d3aa9c14da0b136dfe83f850"
                       "eea9a5f7470427",
                       "Apache-2.0",
                       "https://example.invalid/python-dateutil.whl"),
       createComponent("six", "1.17.0",
                       "4721f391ed90541fddacab5acf947aa0d3dc7d27b2e1e8eda2"
                       "be8970586c3274",
                       "MIT", "https://example.invalid/six.whl"),
       createComponent("tzdata", "2026.5",
                       "b683bd1b6659ddcd810ff02ad09ba821d4bf1065072805063e"
                       "b35c49617905ac",
                       "Apache-2.0", "https://example.invalid/tzdata.whl"),
       createComponent("stockbt", "1", bte::core::sha256("stockbt-source"),
                       "Apache-2.0", "local-build:stockbt")});
  components[3]["licenses"] =
      nlohmann::json::array({"Apache-2.0", "BSD-3-Clause"});
  return nlohmann::json{
      {"architecture", "arm64"},
      {"components", std::move(components)},
      {"installedFiles",
       nlohmann::json::array(
           {{{"path", "python/bin/python3"}, {"sha256", interpreterDigest}},
            {{"path",
              "python/lib/python3.14/site-packages/tzdata/zoneinfo/tzdata.zi"},
             {"sha256", bte::core::sha256("timezone-database-fixture")}}})},
      {"platform", "macos"},
      {"profileBuilderVersion", "1"},
      {"profileId", "macos-arm64-cpython-3.14.9-stockbt-v1"},
      {"pythonAbi", "cp314"},
      {"pythonExecutable", "python/bin/python3"},
      {"pythonVersion", "3.14.9"},
      {"schemaVersion", 1},
      {"stockbtApiVersion", "1"},
      {"stockbtSourceSha256", bte::core::sha256("stockbt-source")},
  };
}

void writeFixtureProfile(const std::filesystem::path &root) {
  const std::string executableBytes = "managed-python-fixture";
  const auto executable = root / "python/bin/python3";
  writeText(executable, executableBytes);
  std::filesystem::permissions(executable,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::add);
  writeText(root /
                "python/lib/python3.14/site-packages/tzdata/zoneinfo/tzdata.zi",
            "timezone-database-fixture");

  const auto manifest = serializeCanonicalJson(
      createFixtureManifest(bte::core::sha256(executableBytes)));
  ASSERT_TRUE(manifest.ok()) << manifest.error().message;
  writeText(root / "manifest.json", manifest.value());
}

void writeCanonicalManifest(const std::filesystem::path &root,
                            const nlohmann::json &manifest) {
  const auto canonical = serializeCanonicalJson(manifest);
  ASSERT_TRUE(canonical.ok()) << canonical.error().message;
  writeText(root / "manifest.json", canonical.value());
}

TEST(RuntimeProfileTest, verifiesCanonicalManifestAndInstalledFileHashes) {
  TemporaryDirectory directory;
  writeFixtureProfile(directory.path());

  const auto verified = verifyRuntimeProfile(directory.path());

  ASSERT_TRUE(verified.ok()) << verified.error().message;
  EXPECT_EQ(verified.value().profileId,
            "macos-arm64-cpython-3.14.9-stockbt-v1");
  EXPECT_EQ(verified.value().pythonVersion, "3.14.9");
  EXPECT_EQ(verified.value().manifestHash.size(), 64U);
  EXPECT_EQ(verified.value().pythonExecutablePath.filename(), "python3");
  EXPECT_EQ(verified.value().timezoneDatabasePath.filename(), "zoneinfo");
}

TEST(RuntimeProfileTest, verifiesHashesForEmptyInstalledFiles) {
  TemporaryDirectory directory;
  writeFixtureProfile(directory.path());
  constexpr std::string_view emptyFilePath =
      "python/lib/python3.14/site-packages/empty.marker";
  writeText(directory.path() / emptyFilePath, "");
  auto manifest =
      createFixtureManifest(bte::core::sha256("managed-python-fixture"));
  manifest["installedFiles"].push_back(
      {{"path", emptyFilePath}, {"sha256", bte::core::sha256("")}});
  writeCanonicalManifest(directory.path(), manifest);

  const auto verified = verifyRuntimeProfile(directory.path());

  ASSERT_TRUE(verified.ok()) << verified.error().message;
  EXPECT_EQ(verified.value().profileId,
            "macos-arm64-cpython-3.14.9-stockbt-v1");
}

TEST(RuntimeProfileTest, rejectsUnmanifestedRuntimeFiles) {
  TemporaryDirectory directory;
  writeFixtureProfile(directory.path());
  writeText(directory.path() /
                "python/lib/python3.14/site-packages/review_unlisted.pth",
            "import os; os.environ['UNMANIFESTED_RUNTIME_FILE'] = 'loaded'\n");

  const auto unlisted = verifyRuntimeProfile(directory.path());

  ASSERT_FALSE(unlisted.ok());
  EXPECT_EQ(unlisted.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);
}

TEST(RuntimeProfileTest, rejectsTamperedFilesAndNonCanonicalManifestBytes) {
  TemporaryDirectory directory;
  writeFixtureProfile(directory.path());
  writeText(directory.path() / "python/bin/python3", "tampered-python");

  const auto tampered = verifyRuntimeProfile(directory.path());
  ASSERT_FALSE(tampered.ok());
  EXPECT_EQ(tampered.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);

  TemporaryDirectory nonCanonicalDirectory;
  writeFixtureProfile(nonCanonicalDirectory.path());
  const auto canonical = serializeCanonicalJson(
      createFixtureManifest(bte::core::sha256("managed-python-fixture")));
  ASSERT_TRUE(canonical.ok());
  writeText(nonCanonicalDirectory.path() / "manifest.json",
            " " + canonical.value());
  const auto nonCanonical = verifyRuntimeProfile(nonCanonicalDirectory.path());
  ASSERT_FALSE(nonCanonical.ok());
  EXPECT_EQ(nonCanonical.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);
}

TEST(RuntimeProfileTest, rejectsTraversalAndDoesNotSearchForSystemPython) {
  TemporaryDirectory directory;
  writeFixtureProfile(directory.path());
  auto manifest =
      createFixtureManifest(bte::core::sha256("managed-python-fixture"));
  manifest["installedFiles"][0]["path"] = "../outside-python";
  const auto canonical = serializeCanonicalJson(manifest);
  ASSERT_TRUE(canonical.ok());
  writeText(directory.path() / "manifest.json", canonical.value());

  const auto traversal = verifyRuntimeProfile(directory.path());
  ASSERT_FALSE(traversal.ok());
  EXPECT_EQ(traversal.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);

  TemporaryDirectory emptyResources;
  const auto unavailable = discoverManagedRuntime(emptyResources.path());
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().code, bte::core::ErrorCode::runtimeUnavailable);
}

TEST(RuntimeProfileTest, rejectsMissingOrSubstitutedPinnedPackages) {
  TemporaryDirectory missingPackageDirectory;
  writeFixtureProfile(missingPackageDirectory.path());
  auto missingManifest =
      createFixtureManifest(bte::core::sha256("managed-python-fixture"));
  missingManifest["components"].erase(1);
  const auto missingBytes = serializeCanonicalJson(missingManifest);
  ASSERT_TRUE(missingBytes.ok());
  writeText(missingPackageDirectory.path() / "manifest.json",
            missingBytes.value());
  const auto missing = verifyRuntimeProfile(missingPackageDirectory.path());
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);

  TemporaryDirectory substitutedPackageDirectory;
  writeFixtureProfile(substitutedPackageDirectory.path());
  auto substitutedManifest =
      createFixtureManifest(bte::core::sha256("managed-python-fixture"));
  substitutedManifest["components"][1]["artifactSha256"] =
      bte::core::sha256("substituted-numpy-wheel");
  const auto substitutedBytes = serializeCanonicalJson(substitutedManifest);
  ASSERT_TRUE(substitutedBytes.ok());
  writeText(substitutedPackageDirectory.path() / "manifest.json",
            substitutedBytes.value());
  const auto substituted =
      verifyRuntimeProfile(substitutedPackageDirectory.path());
  ASSERT_FALSE(substituted.ok());
  EXPECT_EQ(substituted.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);
}

TEST(RuntimeProfileTest, rejectsUnknownComponentLicenseDeclarations) {
  TemporaryDirectory directory;
  writeFixtureProfile(directory.path());
  auto manifest =
      createFixtureManifest(bte::core::sha256("managed-python-fixture"));
  manifest["components"][1]["licenses"].push_back("Unreviewed-Unknown-License");
  const auto canonical = serializeCanonicalJson(manifest);
  ASSERT_TRUE(canonical.ok());
  writeText(directory.path() / "manifest.json", canonical.value());

  const auto unapproved = verifyRuntimeProfile(directory.path());

  ASSERT_FALSE(unapproved.ok());
  EXPECT_EQ(unapproved.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);
}

TEST(RuntimeProfileTest, rejectsAdditionalUnapprovedPackages) {
  TemporaryDirectory directory;
  writeFixtureProfile(directory.path());
  auto manifest =
      createFixtureManifest(bte::core::sha256("managed-python-fixture"));
  manifest["components"].push_back(
      createComponent("pytest", "9.0", bte::core::sha256("pytest-wheel"), "MIT",
                      "https://example.invalid/pytest.whl"));
  const auto canonical = serializeCanonicalJson(manifest);
  ASSERT_TRUE(canonical.ok());
  writeText(directory.path() / "manifest.json", canonical.value());

  const auto extra = verifyRuntimeProfile(directory.path());

  ASSERT_FALSE(extra.ok());
  EXPECT_EQ(extra.error().code, bte::core::ErrorCode::runtimeIntegrityFailure);
}

TEST(RuntimeProfileTest,
     rejectsMalformedProfileMetadataAndDuplicateOrInvalidComponents) {
  using Mutation =
      std::pair<std::string, std::function<void(nlohmann::json &)>>;
  const std::vector<Mutation> mutations{
      {"unsupported platform",
       [](nlohmann::json &manifest) { manifest["platform"] = "linux"; }},
      {"unsupported schema",
       [](nlohmann::json &manifest) { manifest["schemaVersion"] = 2; }},
      {"empty builder version",
       [](nlohmann::json &manifest) {
         manifest["profileBuilderVersion"] = "";
       }},
      {"malformed stockbt source hash",
       [](nlohmann::json &manifest) {
         manifest["stockbtSourceSha256"] = "not-a-sha256";
       }},
      {"empty component list",
       [](nlohmann::json &manifest) {
         manifest["components"] = nlohmann::json::array();
       }},
      {"empty installed-file list",
       [](nlohmann::json &manifest) {
         manifest["installedFiles"] = nlohmann::json::array();
       }},
      {"non-string interpreter path",
       [](nlohmann::json &manifest) { manifest["pythonExecutable"] = 7; }},
      {"missing manifest key",
       [](nlohmann::json &manifest) { manifest.erase("pythonAbi"); }},
      {"additional manifest key",
       [](nlohmann::json &manifest) { manifest["unexpected"] = true; }},
      {"duplicate component",
       [](nlohmann::json &manifest) {
         manifest["components"][1] = manifest["components"][0];
       }},
      {"invalid component digest",
       [](nlohmann::json &manifest) {
         manifest["components"][1]["artifactSha256"] = "ABC";
       }},
      {"empty component name",
       [](nlohmann::json &manifest) {
         manifest["components"][1]["name"] = "";
       }},
      {"empty component version",
       [](nlohmann::json &manifest) {
         manifest["components"][1]["version"] = "";
       }},
      {"empty component URL",
       [](nlohmann::json &manifest) {
         manifest["components"][1]["upstreamUrl"] = "";
       }},
      {"non-string license",
       [](nlohmann::json &manifest) {
         manifest["components"][1]["licenses"] = nlohmann::json::array({1});
       }},
      {"empty license list",
       [](nlohmann::json &manifest) {
         manifest["components"][1]["licenses"] = nlohmann::json::array();
       }},
      {"component with missing key",
       [](nlohmann::json &manifest) {
         manifest["components"][1].erase("upstreamUrl");
       }},
      {"component with additional key",
       [](nlohmann::json &manifest) {
         manifest["components"][1]["unexpected"] = true;
       }},
      {"duplicate approved licenses",
       [](nlohmann::json &manifest) {
         manifest["components"][3]["licenses"] =
             nlohmann::json::array({"Apache-2.0", "Apache-2.0"});
       }},
      {"mismatched pinned version",
       [](nlohmann::json &manifest) {
         manifest["components"][1]["version"] = "2.5.5";
       }},
      {"invalid stockbt component",
       [](nlohmann::json &manifest) {
         manifest["components"][6]["version"] = "2";
       }},
  };

  for (const auto &[label, mutate] : mutations) {
    TemporaryDirectory directory;
    writeFixtureProfile(directory.path());
    auto manifest =
        createFixtureManifest(bte::core::sha256("managed-python-fixture"));
    mutate(manifest);
    writeCanonicalManifest(directory.path(), manifest);

    const auto rejected = verifyRuntimeProfile(directory.path());

    EXPECT_FALSE(rejected.ok()) << label;
    EXPECT_EQ(rejected.error().code,
              bte::core::ErrorCode::runtimeIntegrityFailure)
        << label;
  }
}

TEST(RuntimeProfileTest,
     rejectsInvalidInstalledFileEntriesAndUncoveredRuntimePaths) {
  using Mutation =
      std::pair<std::string, std::function<void(nlohmann::json &)>>;
  const std::vector<Mutation> mutations{
      {"absolute installed-file path",
       [](nlohmann::json &manifest) {
         manifest["installedFiles"][0]["path"] = "/outside/python";
       }},
      {"backslash installed-file path",
       [](nlohmann::json &manifest) {
         manifest["installedFiles"][0]["path"] = "python\\bin\\python3";
       }},
      {"invalid installed-file digest",
       [](nlohmann::json &manifest) {
         manifest["installedFiles"][0]["sha256"] = "wrong";
       }},
      {"non-string installed-file path",
       [](nlohmann::json &manifest) {
         manifest["installedFiles"][0]["path"] = 7;
       }},
      {"duplicate installed-file path",
       [](nlohmann::json &manifest) {
         manifest["installedFiles"][1]["path"] =
             manifest["installedFiles"][0]["path"];
         manifest["installedFiles"][1]["sha256"] =
             manifest["installedFiles"][0]["sha256"];
       }},
      {"missing interpreter file",
       [](nlohmann::json &manifest) {
         manifest["installedFiles"] =
             nlohmann::json::array({manifest["installedFiles"][1]});
       }},
      {"missing timezone database",
       [](nlohmann::json &manifest) {
         manifest["installedFiles"] =
             nlohmann::json::array({manifest["installedFiles"][0]});
       }},
      {"unsafe interpreter path",
       [](nlohmann::json &manifest) {
         manifest["pythonExecutable"] = "../python";
       }},
      {"component path segment",
       [](nlohmann::json &manifest) {
         manifest["installedFiles"][0]["path"] = "python/./bin/python3";
       }},
      {"missing installed file",
       [](nlohmann::json &manifest) {
         manifest["installedFiles"][0]["path"] = "python/bin/missing";
       }},
      {"file digest mismatch",
       [](nlohmann::json &manifest) {
         manifest["installedFiles"][0]["sha256"] = bte::core::sha256("other");
       }},
  };

  for (const auto &[label, mutate] : mutations) {
    TemporaryDirectory directory;
    writeFixtureProfile(directory.path());
    auto manifest =
        createFixtureManifest(bte::core::sha256("managed-python-fixture"));
    mutate(manifest);
    writeCanonicalManifest(directory.path(), manifest);

    const auto rejected = verifyRuntimeProfile(directory.path());

    EXPECT_FALSE(rejected.ok()) << label;
    EXPECT_EQ(rejected.error().code,
              bte::core::ErrorCode::runtimeIntegrityFailure)
        << label;
  }
}

TEST(RuntimeProfileTest, rejectsNonExecutableInterpreterAndMissingManifest) {
  TemporaryDirectory directory;
  writeFixtureProfile(directory.path());
  std::filesystem::permissions(directory.path() / "python/bin/python3",
                               std::filesystem::perms::owner_read,
                               std::filesystem::perm_options::replace);

  const auto nonExecutable = verifyRuntimeProfile(directory.path());

  ASSERT_FALSE(nonExecutable.ok());
  EXPECT_EQ(nonExecutable.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);

  TemporaryDirectory missingManifest;
  std::filesystem::create_directories(missingManifest.path());
  const auto missing = verifyRuntimeProfile(missingManifest.path());
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);
}

TEST(RuntimeProfileTest, rejectsOversizedManifestBeforeParsing) {
  TemporaryDirectory directory;
  writeText(directory.path() / "manifest.json",
            std::string(1024U * 1024U + 1U, 'x'));

  const auto rejected = verifyRuntimeProfile(directory.path());

  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);
}

TEST(RuntimeProfileTest, rejectsMissingProfileRootAndMalformedManifest) {
  TemporaryDirectory directory;
  const auto missingRoot = verifyRuntimeProfile(directory.path() / "missing");
  ASSERT_FALSE(missingRoot.ok());
  EXPECT_EQ(missingRoot.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);

  writeText(directory.path() / "manifest.json", "{malformed");
  const auto malformed = verifyRuntimeProfile(directory.path());
  ASSERT_FALSE(malformed.ok());
  EXPECT_EQ(malformed.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);
}

#if defined(__unix__) || defined(__APPLE__)
TEST(RuntimeProfileTest, rejectsUnsupportedFilesystemEntries) {
  TemporaryDirectory directory;
  writeFixtureProfile(directory.path());
  const auto fifo = directory.path() / "unsupported-entry";
  ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);

  const auto rejected = verifyRuntimeProfile(directory.path());

  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);
}
#endif

TEST(RuntimeProfileTest, discoversTheInstalledProfileFromApplicationResources) {
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
  TemporaryDirectory resources;
  const auto missing = discoverManagedRuntime(resources.path());
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, bte::core::ErrorCode::runtimeUnavailable);

  const auto profileRoot =
      resources.path() / "RuntimeProfiles" /
      std::string{bte::bindings::python::managedRuntimeProfileId};
  writeFixtureProfile(profileRoot);

  const auto discovered = discoverManagedRuntime(resources.path());

  ASSERT_TRUE(discovered.ok()) << discovered.error().message;
  EXPECT_EQ(discovered.value().root, std::filesystem::canonical(profileRoot));
#else
  GTEST_SKIP() << "Managed runtime discovery requires macOS arm64.";
#endif
}

TEST(RuntimeProfileTest, rejectsInstalledFileSymlinksEscapingTheProfile) {
  TemporaryDirectory directory;
  TemporaryDirectory outside;
  writeFixtureProfile(directory.path());
  const auto outsideFile = outside.path() / "external-python";
  writeText(outsideFile, "managed-python-fixture");
  const auto executable = directory.path() / "python/bin/python3";
  std::filesystem::remove(executable);
  std::filesystem::create_symlink(outsideFile, executable);
  auto manifest =
      createFixtureManifest(bte::core::sha256("managed-python-fixture"));
  writeCanonicalManifest(directory.path(), manifest);

  const auto rejected = verifyRuntimeProfile(directory.path());

  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code,
            bte::core::ErrorCode::runtimeIntegrityFailure);
}

} // namespace
