# Onboarding

Goal: get a new contributor (human or AI) from clone to first green local build in **under 60 minutes**, and to first merged PR in their first week.

---

## before you write any code

### Read

In this order:

1. `README.md` — what this is.
2. [`Governance/AGENTS.md`](Governance/AGENTS.md) — how to behave (applies to humans too).
3. [`Governance/CONTRIBUTING.md`](Governance/CONTRIBUTING.md) — process.
4. [`Specs/00Overview.md`](Specs/00Overview.md) and [`Specs/README.md`](Specs/README.md) — system design.
5. [`TeamOwnershipAndProductPillars.md`](TeamOwnershipAndProductPillars.md) — seven topic owners and the three product pillars (organizational; optional on day one).
6. `Docs/DefinitionOfDone.md` — what "done" means.

If you only have time to read three, read [`Governance/AGENTS.md`](Governance/AGENTS.md), [`Specs/00Overview.md`](Specs/00Overview.md), and [`DefinitionOfDone.md`](DefinitionOfDone.md).

### Get access

Ask the repo lead for:

- Repository write access (GitHub).
- Membership in the relevant team in `.github/CODEOWNERS`.
- Sync-chat invitation.
- A Databento API key (only if you'll work on the data pipeline; otherwise the existing CSV snapshots are enough).

---

## local environment (target: 60 minutes)

### Prerequisites by OS

#### macOS (Apple Silicon or Intel)

```bash
# Xcode command-line tools
xcode-select --install

# Homebrew, if you don't have it
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Build tools
brew install cmake ninja git pre-commit
brew install --cask qt    # OR install Qt 6.8 LTS via the Qt online installer
brew install python@3.11
```

#### Linux (Ubuntu 22.04+ / Fedora / Arch)

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install -y build-essential cmake ninja-build git pre-commit \
                    clang clang-tidy clang-format llvm \
                    qt6-base-dev qt6-charts-dev \
                    python3.11 python3.11-venv pipx
```

For other distros, install the equivalents. Clang 17+ is preferred; GCC 13+ also works.

#### Windows

Install in this order:

1. [Visual Studio 2022](https://visualstudio.microsoft.com/) with **Desktop development with C++** workload.
2. [Git for Windows](https://git-scm.com/).
3. [CMake 3.24+](https://cmake.org/download/) (the VS installer's bundled CMake works too).
4. [Qt 6.8 LTS](https://www.qt.io/download-qt-installer) — pick the MSVC 2022 64-bit build.
5. [Python 3.11+](https://www.python.org/downloads/).

Use a "Developer PowerShell for VS 2022" terminal for builds so the MSVC toolchain is on PATH.

### Clone and bootstrap

```bash
git clone <repo-url>
cd Stock-Back-Test-System

# Submodules (vcpkg, etc., once C++ source lands)
git submodule update --init

# Pre-commit hooks (run locally on every git push)
pre-commit install --hook-type pre-push --hook-type pre-commit

# Python pipeline (already in this repo)
python3.11 -m venv .venv
source .venv/bin/activate                    # Windows: .venv\Scripts\activate
pip install -r requirements.txt

# C++ build (see Docs/BUILD.md + Docs/Specs/09)
./RunTest.sh                                 # configure + build + unit tests (same as below)
# or manually:
cmake --preset dev
cmake --build --preset dev
ctest --preset dev                           # unit tests
# Optional: ASan/UBSan on Clang/GNU — cmake --preset dev-sanitize …
```

### Verify

If `ctest --preset dev` passes, your environment is good. For ASan/UBSan, configure and run `dev-sanitize` (see [`Docs/BUILD.md`](BUILD.md)). Run the existing Python pipeline once to verify it too:

```bash
# (Optional) collect data into DuckDB — needs Databento API key
./DataFetcher/collectData.sh

# Or just extract CSVs from an existing DuckDB
./DataFetcher/extractFromDB.sh
```

If anything in this section fails, fix the docs as part of your first PR — that's everyone's first contribution.

---

## first PR

Pick a task from the issue tracker labeled `good-first-issue`. If none, the lead will assign one.

Good first PRs (by category):

- **Docs**: clarify a section in a spec, fix a typo, expand an example.
- **Tests**: add tests for a public symbol that isn't yet covered. Run `Tools/BteSymbolAudit.py` to find candidates.
- **Tooling**: add a missing helper script, improve `pre-commit-config.yaml`.

For your first PR, **prefer tests or docs** over production code. It's a low-risk way to learn the review process, the CI gates, and the team's review style.

Workflow recap:

```bash
git switch -c feature/my-first-pr
# ... make changes ...
git add -p
git commit -m "test(indicators): cover RSI cold-start invariant"
git push -u origin HEAD
gh pr create   # or use the GitHub UI
```

Self-review using `Docs/ReviewPlaybook.md` **before** requesting review.

---

## checkpoint

To contribute you should have:

- [ ] Local dev environment producing a clean `ctest --preset dev`.
- [ ] At least one merged PR (any size, any kind).
- [ ] Read every doc in `Docs/`.
- [ ] Read the Specs that touch your area of work.
- [ ] Skimmed the five repository-specific C++ skills in [`.agents/skills/`](../.agents/skills/README.md) so you know what they enforce.

If any are blocked, raise it in the next sync.

---

## Common stumbling blocks

| Symptom                                         | Likely cause                               | Fix                                                                              |
| ----------------------------------------------- | ------------------------------------------ | -------------------------------------------------------------------------------- |
| `cmake --preset dev` not found                  | Old CMake                                  | Need 3.24+; `brew upgrade cmake` / install fresh                                 |
| Sanitizer reports leak in third-party lib       | Suppression missing                        | Add narrow entry to `Tests/sanitizer-suppressions.txt`, get CODEOWNER review     |
| `clang-tidy` flagging hundreds of legacy issues | Running on whole repo                      | Use `Tools/RunClangTidyDiff.sh` for changed files only                        |
| Qt not found by CMake                           | Qt install path not on `CMAKE_PREFIX_PATH` | Set `CMAKE_PREFIX_PATH` env var or `-DCMAKE_PREFIX_PATH=...`                     |
| Python pipeline can't find DuckDB               | Missing dep                                | `pip install -r requirements.txt` inside `.venv`                                 |
| `pre-commit` slow on huge diffs                 | Auditing everything                        | It only runs on changed files; if you rebased, expect more work briefly          |
| Tests pass locally, fail in CI on Windows       | Path / line-ending / case-sensitivity      | Use forward slashes in code paths; check `.gitattributes` for `text=auto eol=lf` |
| "Permission denied" running shell scripts       | Missing exec bit                           | `chmod +x DataFetcher/*.sh` (and check it's preserved in your commit)            |

---

## Who to ask

- **Build / CI** issues → repo lead (named in `.github/CODEOWNERS`).
- **Spec ambiguity** → comment on the spec file in a PR or issue. Don't DM — the answer should be public.
- **Anything urgent** → sync chat.

Welcome aboard.
