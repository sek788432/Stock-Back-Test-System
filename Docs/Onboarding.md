# Onboarding

Goal: get a contributor from a clean clone to a verified local build using only
commands and dependencies present in this checkout.

---

## before you write any code

### Read

In this order:

1. `README.md` — what this is.
2. [`Governance/AGENTS.md`](Governance/AGENTS.md) — how to behave (applies to humans too).
3. [`Governance/CONTRIBUTING.md`](Governance/CONTRIBUTING.md) — process.
4. [`Specs/Overview.md`](Specs/Overview.md) and [`Specs/README.md`](Specs/README.md) — system design.
5. [`DefinitionOfDone.md`](DefinitionOfDone.md) — what "done" means.

If you only have time to read three, read [`Governance/AGENTS.md`](Governance/AGENTS.md), [`Specs/Overview.md`](Specs/Overview.md), and [`DefinitionOfDone.md`](DefinitionOfDone.md).

### Access

Repository write access is optional for local work; use a fork when you are not
an invited collaborator. A personal Databento API key is required only for
developer ingestion. Existing tracked CSV snapshots are sufficient for the C++
build and registered tests.

---

## local environment

### Prerequisites by OS

#### macOS (Apple Silicon or Intel)

```bash
# Xcode command-line tools
xcode-select --install

# Install Homebrew from https://brew.sh/ first when `brew` is unavailable.
brew install cmake ninja git
brew install python
```

The backend-only `dev` preset does not require Qt. For `qt-dev`, install Qt
6.8 or newer (including Charts) with the Qt online installer. CI currently
tests Qt 6.9.x; see [`.github/workflows/ci.yml`](../.github/workflows/ci.yml).

#### Linux (Ubuntu 24.04+ / Fedora / Arch)

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install -y build-essential cmake ninja-build git \
                    clang-18 clang-tidy-18 clang-tools-18 clang-format-18 \
                    cppcheck iwyu llvm \
                    python3 python3-venv
```

For other distros, install the equivalents. The project requires a C++20
compiler; CI's analyzer jobs use Clang 18. The backend-only `dev` preset does
not require Qt. For `qt-dev`,
install Qt 6.8 or newer (including Charts) with the Qt online installer; do not
assume a distribution's `qt6-*` packages satisfy that minimum without checking
their version.

The coverage job also uses the pinned Python tools `gcovr==8.5` and
`diff-cover==10.0.0`. See [`Specs/CiDevFlow.md`](Specs/CiDevFlow.md) §7 for
the analyzer and coverage commands.

#### Windows

Windows packaging is an approved target, but Windows CI and a verified local
onboarding workflow are still **Planned**. Do not treat the Unix Makefiles
presets below as a supported Windows recipe. Use a current macOS or Ubuntu
environment until a checked-in Windows preset and registered CI job exist.

### Clone

```bash
# Invited collaborators may clone the canonical repository directly.
git clone https://github.com/sek788432/Stock-Back-Test-System.git
cd Stock-Back-Test-System
```

Fork contributors should clone their GitHub fork as `origin` instead (replace
`YOUR-GITHUB-NAME` below), then keep the canonical repository as read-only
`upstream`:

```bash
git clone https://github.com/YOUR-GITHUB-NAME/Stock-Back-Test-System.git
cd Stock-Back-Test-System
git remote add upstream https://github.com/sek788432/Stock-Back-Test-System.git
```

The `git push` command in the first-PR workflow targets the contributor-owned
`origin`.

### Bootstrap

From either clone:

```bash
# Python pipeline (already in this repo)
python3 -m venv .venv
source .venv/bin/activate
pip install -r DataFetcher/requirements.txt

# C++ build (see Docs/BUILD.md + Docs/Specs/BuildDistribution.md)
./RunTest.sh                                 # all registered backend and Qt tests
# or manually, matching the current CI Qt build:
cmake --preset qt-dev -DBTE_BUILD_TESTS=ON -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build --preset qt-dev --parallel
ctest --preset qt-dev --no-tests=error
# smaller non-Qt development build:
cmake --preset dev
cmake --build --preset dev
ctest --preset dev                           # unit tests

# Before pushing C++ changes or requesting CI:
./RunTest.sh
bte_quality_base=origin/main                # Fork clone: upstream/main
./RunQuality.sh --base "$bte_quality_base" --head HEAD
```

Fetch the selected base remote immediately before the final committed-revision
check.

`RunQuality.sh` is a one-shot command. On its first run it uses Homebrew on
macOS or `apt` on Ubuntu to install missing analyzers and Python 3.12, then
creates a private hash-locked environment below `Output/QualityVenv/`.
Subsequent runs reuse it; no shell activation or `PATH` configuration is needed.

### Verify

If `ctest --preset dev` passes, your environment is good. The ASan/UBSan and
TSan commands are in [`BUILD.md`](BUILD.md). Running the data pipeline is
optional and requires its own credentials:

```bash
# (Optional) collect data into DuckDB — needs DATABENTO_API_KEY
python3 DataFetcher/FetchDatabento.py

# Or just extract CSVs from an existing DuckDB
python3 DataFetcher/GetFromDb.py
```

If anything in this section fails, fix the docs as part of your first PR — that's everyone's first contribution.

---

## first PR

Pick a task from the issue tracker labeled `good first issue`, or open a focused
documentation/test proposal when none is available.

Good first PRs (by category):

- **Docs**: clarify a section in a spec, fix a typo, expand an example.
- **Tests**: add positive, negative, and boundary coverage for an existing public behavior whose current tests are incomplete. Find a candidate by comparing a relevant public API with its registered tests.
- **Tooling**: add tests for or improve an existing checked-in helper such as
  `Tools/CheckProjectStandards.py`.

For your first PR, **prefer tests or docs** over production code. It is a
low-risk way to learn the review process and CI gates.

Workflow recap:

```bash
# Direct canonical clone:
git fetch origin
bte_branch_base=origin/main
# Fork clone: run `git fetch upstream` and use `bte_branch_base=upstream/main`.
git switch -c feature/my-first-pr "$bte_branch_base"
# ... make changes ...
git add -p
git commit -m "test(indicators): cover RSI cold-start invariant"
git push -u origin HEAD
# Open a pull request from the pushed branch in the GitHub UI.
```

Self-review using `Docs/ReviewPlaybook.md` **before** requesting review.

---

## checkpoint

Before requesting review you should have:

- [ ] Local dev environment producing a clean `ctest --preset dev`.
- [ ] Read the canonical governance file, Definition of Done, and the specs that
      touch your work.
- [ ] Read every project skill whose description matches the change.

Record any blocked check in the pull request instead of silently omitting it.

---

## Common stumbling blocks

| Symptom                                         | Likely cause                               | Fix                                                                              |
| ----------------------------------------------- | ------------------------------------------ | -------------------------------------------------------------------------------- |
| `cmake --preset dev` not found                  | Old CMake                                  | Need 3.24+; `brew upgrade cmake` / install fresh                                 |
| Sanitizer reports a finding                     | Runtime defect or unsupported dependency   | Treat it as a defect; no sanitizer-suppression file is currently wired into the build |
| `clang-tidy` reports a project finding          | Required analysis found a rule violation   | Fix it or document a narrow approved suppression; run the exact CI specification command  |
| Qt not found by CMake                           | Qt install path not on `CMAKE_PREFIX_PATH` | Set `CMAKE_PREFIX_PATH` env var or `-DCMAKE_PREFIX_PATH=...`                     |
| Python pipeline can't find DuckDB               | Missing dep                                | `pip install -r DataFetcher/requirements.txt` inside `.venv`                     |
| First configure cannot fetch GoogleTest         | Network unavailable                         | Restore network access or use an already populated FetchContent cache            |
| Tests pass locally, fail on another OS          | Path / line-ending / case-sensitivity       | Use `std::filesystem::path`; check `.gitattributes` for line-ending policy        |

---

## Who to ask

- **Build / CI** issues → open an issue or PR discussion with the exact command
  and output.
- **Spec ambiguity** → comment on the owning spec in a PR or issue so the answer
  remains discoverable.

Welcome aboard.
