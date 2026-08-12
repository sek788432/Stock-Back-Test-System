#!/usr/bin/env bash
# Run the merge-blocking static-analysis and changed-coverage checks locally.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

base="origin/main"
head="HEAD"
run_scan_build=1
operating_system="${BTE_QUALITY_OS:-$(uname -s)}"

usage() {
  echo "Usage: $0 [--base <revision>] [--head <revision>] [--fast]"
  echo "  Runs clang-tidy, cppcheck, IWYU, scan-build, and changed-code coverage."
  echo "  --fast skips the slower whole-tree scan-build pass."
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base)
      [[ $# -ge 2 ]] || { echo "--base requires a revision" >&2; exit 2; }
      base="$2"
      shift 2
      ;;
    --head)
      [[ $# -ge 2 ]] || { echo "--head requires a revision" >&2; exit 2; }
      head="$2"
      shift 2
      ;;
    --fast)
      run_scan_build=0
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

resolve_revision() {
  git rev-parse --verify "$1^{commit}" 2>/dev/null || {
    echo "Revision does not resolve to a commit: $1" >&2
    exit 2
  }
}

base_commit="$(resolve_revision "$base")"
head_commit="$(resolve_revision "$head")"
checked_out_commit="$(git rev-parse --verify HEAD)"
if [[ "$head_commit" != "$checked_out_commit" ]]; then
  echo "--head must resolve to the checked-out HEAD ($checked_out_commit)." >&2
  exit 2
fi
if [[ -n "$(git status --porcelain --untracked-files=all)" ]]; then
  echo "RunQuality.sh requires a clean working tree so results describe the committed HEAD." >&2
  exit 2
fi
base="$base_commit"
head="$head_commit"

find_command() {
  local candidate
  for candidate in "$@"; do
    if command -v "$candidate" >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  done
  echo "Required command not found: $*" >&2
  return 2
}

has_command() {
  local candidate
  for candidate in "$@"; do
    if command -v "$candidate" >/dev/null 2>&1; then
      return 0
    fi
  done
  return 1
}

has_llvm_18_toolchain() {
  case "$operating_system" in
    Darwin)
      has_command clang clang++ clang-tidy && \
        clang --version 2>/dev/null | grep -Eq 'version 18([. ]|$)'
      ;;
    Linux)
      has_command clang-18 clang++-18 clang-tidy-18
      ;;
    *)
      return 1
      ;;
  esac
}

prepend_homebrew_paths() {
  if ! command -v brew >/dev/null 2>&1; then
    return
  fi
  local formula_prefix
  formula_prefix="$(brew --prefix llvm@18 2>/dev/null || true)"
  if [[ -n "$formula_prefix" ]]; then
    PATH="$formula_prefix/bin:$PATH"
  fi
  formula_prefix="$(brew --prefix python@3.12 2>/dev/null || true)"
  if [[ -n "$formula_prefix" ]]; then
    PATH="$formula_prefix/bin:$PATH"
  fi
  export PATH
}

bootstrap_system_tools() {
  local packages=()
  if ! has_llvm_18_toolchain || \
     { [[ "$run_scan_build" -eq 1 ]] && ! has_command scan-build-18 scan-build; }; then
    if [[ "$operating_system" == "Darwin" ]]; then
      packages+=(llvm@18)
    else
      packages+=(llvm)
    fi
  fi
  if ! has_command cppcheck; then
    packages+=(cppcheck)
  fi
  if ! has_command iwyu_tool.py iwyu_tool; then
    packages+=(include-what-you-use)
  fi

  if [[ "${#packages[@]}" -eq 0 ]]; then
    return
  fi

  case "$operating_system" in
    Darwin)
      if ! command -v brew >/dev/null 2>&1; then
        echo "Homebrew is required to bootstrap local quality tools on macOS." >&2
        exit 2
      fi
      echo "== First-run setup: installing ${packages[*]} with Homebrew =="
      brew install "${packages[@]}"
      prepend_homebrew_paths
      ;;
    Linux)
      if ! command -v apt-get >/dev/null 2>&1; then
        echo "Automatic quality-tool setup currently supports apt-based Linux distributions." >&2
        exit 2
      fi
      local apt_prefix=()
      if [[ "$(id -u)" -ne 0 ]]; then
        apt_prefix+=(sudo)
      fi
      echo "== First-run setup: installing quality tools with apt =="
      "${apt_prefix[@]}" apt-get update
      "${apt_prefix[@]}" apt-get install --yes \
        clang-18 clang-tidy-18 clang-tools-18 cppcheck iwyu python3.12 python3.12-venv
      ;;
    *)
      echo "Automatic quality-tool setup is not supported on $operating_system." >&2
      exit 2
      ;;
  esac
}

find_python_312() {
  local candidate resolved
  for candidate in python3.12 python3; do
    resolved="$(command -v "$candidate" 2>/dev/null || true)"
    if [[ -n "$resolved" ]] && "$resolved" -c \
      'import sys; raise SystemExit(sys.version_info[:2] != (3, 12))'; then
      echo "$resolved"
      return 0
    fi
  done
  return 1
}

bootstrap_python_tools() {
  if [[ -n "${BTE_QUALITY_PYTHON:-}" && -n "${BTE_QUALITY_DIFF_COVER:-}" ]]; then
    quality_python="$BTE_QUALITY_PYTHON"
    diff_cover="$BTE_QUALITY_DIFF_COVER"
    return
  fi

  local python_312
  python_312="$(find_python_312 || true)"
  if [[ -z "$python_312" ]]; then
    case "$operating_system" in
      Darwin)
        if ! command -v brew >/dev/null 2>&1; then
          echo "Homebrew is required to bootstrap Python 3.12 on macOS." >&2
          exit 2
        fi
        echo "== First-run setup: installing Python 3.12 with Homebrew =="
        brew install python@3.12
        prepend_homebrew_paths
        python_312="$(find_python_312 || true)"
        ;;
      Linux)
        local apt_prefix=()
        if [[ "$(id -u)" -ne 0 ]]; then
          apt_prefix+=(sudo)
        fi
        echo "== First-run setup: installing Python 3.12 with apt =="
        "${apt_prefix[@]}" apt-get update
        "${apt_prefix[@]}" apt-get install --yes python3.12 python3.12-venv
        python_312="$(find_python_312 || true)"
        ;;
    esac
  fi
  if [[ -z "$python_312" ]]; then
    echo "Python 3.12 could not be bootstrapped." >&2
    exit 2
  fi

  local quality_environment="Output/QualityVenv"
  local requirements="Tools/CoverageRequirements.txt"
  local requirements_stamp="$quality_environment/CoverageRequirements.txt"
  if [[ ! -x "$quality_environment/bin/python" ]] || \
     [[ ! -x "$quality_environment/bin/diff-cover" ]] || \
     ! cmp -s "$requirements" "$requirements_stamp"; then
    echo "== First-run setup: creating the locked coverage environment =="
    "$python_312" -m venv "$quality_environment"
    "$quality_environment/bin/python" -m pip install \
      --disable-pip-version-check \
      --only-binary=:all: \
      --require-hashes \
      --requirement "$requirements"
    cp "$requirements" "$requirements_stamp"
  fi
  quality_python="$quality_environment/bin/python"
  diff_cover="$quality_environment/bin/diff-cover"
}

if [[ -z "${BTE_QUALITY_PYTHON:-}" || -z "${BTE_QUALITY_DIFF_COVER:-}" ]]; then
  prepend_homebrew_paths
fi
bootstrap_system_tools
bootstrap_python_tools

find_command cmake >/dev/null
find_command ctest >/dev/null
find_command clang-tidy-18 clang-tidy >/dev/null
find_command cppcheck >/dev/null
find_command iwyu_tool.py iwyu_tool >/dev/null
clang_compiler="$(find_command clang-18 clang)"
clangxx_compiler="$(find_command clang++-18 clang++)"

scan_build=""
if [[ "$run_scan_build" -eq 1 ]]; then
  scan_build="$(find_command scan-build-18 scan-build)"
fi

"$quality_python" -c 'import gcovr' >/dev/null 2>&1 || {
  echo "The bootstrapped quality environment does not contain gcovr." >&2
  exit 2
}

echo "== Static analysis: ${base}..${head} =="
analysis_arguments=(-DBTE_BUILD_QT_APP=ON)
if [[ "$operating_system" == "Darwin" ]]; then
  analysis_arguments+=("-DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path)")
fi
cmake -E remove_directory Output/analysis
CC="$clang_compiler" CXX="$clangxx_compiler" \
  cmake --preset analysis "${analysis_arguments[@]}"
cmake --build --preset analysis --parallel
"$quality_python" Tools/RunStaticAnalysis.py clang-tidy --base "$base" --head "$head"
"$quality_python" Tools/RunStaticAnalysis.py cppcheck --base "$base" --head "$head"
"$quality_python" Tools/RunStaticAnalysis.py iwyu --base "$base" --head "$head"

if [[ "$run_scan_build" -eq 1 ]]; then
  cmake -E remove_directory Output/scan-build
  cmake -E remove_directory Output/scan-build-reports
  "$scan_build" --use-analyzer="$clang_compiler" \
    cmake -S . -B Output/scan-build -DBTE_BUILD_TESTS=OFF -DBTE_BUILD_QT_APP=ON
  "$scan_build" --use-analyzer="$clang_compiler" \
    --status-bugs --keep-empty -o Output/scan-build-reports \
    cmake --build Output/scan-build --parallel
fi

echo "== Changed-code coverage: ${base}..${head} =="
cmake -E remove_directory Output/coverage
cmake --preset coverage -DBTE_BUILD_QT_APP=ON
cmake --build --preset coverage --parallel
ctest --preset coverage --no-tests=error

report_directory="Output/CoverageReport"
cmake -E make_directory "$report_directory"
gcov_arguments=()
if [[ -n "${BTE_GCOV_EXECUTABLE:-}" ]]; then
  gcov_arguments+=(--gcov-executable "$BTE_GCOV_EXECUTABLE")
elif [[ "$operating_system" == "Darwin" ]]; then
  gcov_arguments+=(--gcov-executable "xcrun llvm-cov gcov")
fi

"$quality_python" -m gcovr \
  --root . \
  --filter 'Src/' \
  --exclude 'Output/' \
  --merge-mode-functions merge-use-line-min \
  --decisions \
  --cobertura "$report_directory/coverage.xml" \
  --json "$report_directory/coverage.json" \
  --markdown-summary "$report_directory/summary.md" \
  --html-details "$report_directory/index.html" \
  "${gcov_arguments[@]}"

"$diff_cover" "$report_directory/coverage.xml" --compare-branch="$base" --fail-under=98
"$quality_python" Tools/CheckDiffBranchCoverage.py \
  "$report_directory/coverage.json" \
  --base "$base" \
  --head "$head" \
  --fail-under 90

echo "Quality checks passed. Coverage report: $report_directory/index.html"
