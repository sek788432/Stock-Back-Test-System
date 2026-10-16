#!/usr/bin/env bash
# Rebuild the warning-clean Qt desktop application and launch it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

if [[ -n "${BTE_DATA_STORE:-}" && -z "${BTE_DATA_SNAPSHOT_ID:-}" ]] ||
  [[ -z "${BTE_DATA_STORE:-}" && -n "${BTE_DATA_SNAPSHOT_ID:-}" ]]; then
  echo "BTE_DATA_STORE and BTE_DATA_SNAPSHOT_ID must be set together" >&2
  exit 2
fi
if [[ -n "${BTE_DATA_SNAPSHOT_ID:-}" &&
  ! "$BTE_DATA_SNAPSHOT_ID" =~ ^[0-9a-f]{64}$ ]]; then
  echo "BTE_DATA_SNAPSHOT_ID must be 64 lowercase hex" >&2
  exit 2
fi
if [[ -n "${BTE_DATA_STORE:-}" &&
  ! -f "$BTE_DATA_STORE/Snapshots/$BTE_DATA_SNAPSHOT_ID/Manifest.btesnapshot" ]]; then
  echo "snapshot manifest does not exist in BTE_DATA_STORE" >&2
  exit 2
fi

cmake --preset qt-dev \
  -DBTE_BUILD_TESTS=ON \
  -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build --preset qt-dev --parallel

if [[ -z "${BTE_DATA_STORE:-}" ]]; then
  readonly EMPTY_SHA256="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
  readonly SNAPSHOT_BUILDER="$ROOT/Output/qt-dev/Src/Backend/Data/bte_snapshot_builder"
  export BTE_DATA_STORE="$ROOT/Output/DevData"
  BTE_DATA_SNAPSHOT_ID="$("$SNAPSHOT_BUILDER" \
    "$ROOT/StockData/Extracted" \
    "$BTE_DATA_STORE" \
    "$EMPTY_SHA256" \
    "$EMPTY_SHA256" \
    AAPL MSFT NVDA)"
  export BTE_DATA_SNAPSHOT_ID

  if [[ ! "$BTE_DATA_SNAPSHOT_ID" =~ ^[0-9a-f]{64}$ ]]; then
    echo "snapshot builder returned an invalid ID" >&2
    exit 2
  fi
  if [[ ! -f "$BTE_DATA_STORE/Snapshots/$BTE_DATA_SNAPSHOT_ID/Manifest.btesnapshot" ]]; then
    echo "snapshot builder did not publish the selected manifest" >&2
    exit 2
  fi
fi

readonly APP="$ROOT/Output/qt-dev/Src/App/stockBacktester"
if [[ ! -x "$APP" ]]; then
  echo "built application is missing or not executable: $APP" >&2
  exit 2
fi

if COMMIT="$(git -C "$ROOT" rev-parse --verify HEAD 2>/dev/null)"; then
  BRANCH="$(git -C "$ROOT" symbolic-ref --short -q HEAD 2>/dev/null || printf 'detached')"
  if STATUS="$(git -C "$ROOT" status --porcelain --untracked-files=normal 2>/dev/null)"; then
    if [[ -n "$STATUS" ]]; then
      DIRTY="yes"
    else
      DIRTY="no"
    fi
  else
    DIRTY="unknown"
  fi
else
  COMMIT="unavailable"
  BRANCH="unavailable"
  DIRTY="unknown"
fi

if command -v shasum >/dev/null 2>&1; then
  APP_DIGEST="$(shasum -a 256 "$APP")"
elif command -v sha256sum >/dev/null 2>&1; then
  APP_DIGEST="$(sha256sum "$APP")"
else
  echo "a SHA-256 checksum tool is required to identify the built app" >&2
  exit 2
fi
APP_DIGEST="${APP_DIGEST%% *}"

export BTE_LAUNCH_CHECKOUT="$ROOT"
export BTE_LAUNCH_BRANCH="$BRANCH"
export BTE_LAUNCH_COMMIT="$COMMIT"
export BTE_LAUNCH_DIRTY="$DIRTY"
export BTE_LAUNCH_APP_SHA256="$APP_DIGEST"

printf 'Checkout: %s\nBranch: %s\nCommit: %s\nDirty: %s\nApp SHA-256: %s\nSnapshot ID: %s\n' \
  "$ROOT" "$BRANCH" "$COMMIT" "$DIRTY" "$APP_DIGEST" "$BTE_DATA_SNAPSHOT_ID"
exec "$APP"
