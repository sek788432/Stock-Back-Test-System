#!/usr/bin/env bash
# Enforce both changed-code coverage gates and publish their combined evidence.
set -euo pipefail

if [[ $# -ne 6 ]]; then
  echo "Usage: $0 <coverage.xml> <coverage.json> <base> <head> <commit> <summary.md>" >&2
  exit 2
fi

coverage_xml="$1"
coverage_json="$2"
base="$3"
head="$4"
commit="$5"
summary="$6"
line_output="changed-line-coverage.txt"
branch_output="changed-branch-coverage.txt"

line_status=0
branch_status=0
diff-cover "$coverage_xml" --compare-branch="$base" --fail-under=98 \
  | tee "$line_output" || line_status=$?
python3 Tools/CheckDiffBranchCoverage.py "$coverage_json" \
  --base "$base" --head "$head" --fail-under 90 \
  | tee "$branch_output" || branch_status=$?

{
  cat "$summary"
  echo ""
  echo "### Changed-code gates"
  echo '```text'
  cat "$line_output"
  cat "$branch_output"
  echo '```'
  echo "Report commit: \`$commit\`"
} >> "${GITHUB_STEP_SUMMARY:-/dev/stdout}"

if [[ "$line_status" -ne 0 || "$branch_status" -ne 0 ]]; then
  exit 1
fi
