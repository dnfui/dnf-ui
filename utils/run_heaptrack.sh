#!/usr/bin/env bash
set -e

# Record app heap allocations and write a text report before leaving the build environment.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_BIN="$(realpath "$1")"

if ! command -v heaptrack >/dev/null || ! command -v heaptrack_print >/dev/null; then
  echo "*** Install Heaptrack before profiling DNF UI ***" >&2
  exit 1
fi

PROFILE_DIR="${2:-}"
if [ -z "$PROFILE_DIR" ]; then
  mkdir -p "$PROJECT_ROOT/build/heaptrack"
  PROFILE_DIR="$(mktemp -d "$PROJECT_ROOT/build/heaptrack/run.XXXXXXXX")"
fi

echo "*** Starting DNF UI under Heaptrack ***"
umask 022
# Heaptrack 1.5.0 does not handle spaces in explicit output paths.
(
  cd "$PROFILE_DIR"
  heaptrack --record-only "$APP_BIN"
)

shopt -s nullglob
profiles=("$PROFILE_DIR"/heaptrack.dnfui.*)
if [ "${#profiles[@]}" -ne 1 ]; then
  echo "*** Expected one Heaptrack recording in $PROFILE_DIR ***" >&2
  exit 1
fi

echo "*** Writing Heaptrack report ***"
heaptrack_print -f "${profiles[0]}" -l > "$PROFILE_DIR/report.txt"
echo "*** Heaptrack recording and report saved in $PROFILE_DIR ***"
