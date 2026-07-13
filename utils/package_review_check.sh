#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SPEC_FILE="${SPEC_FILE:-$PROJECT_ROOT/dnf-ui.spec}"
MOCK_CONFIG="${MOCK_CONFIG:-fedora-rawhide-x86_64}"
REVIEW_TOPDIR="${REVIEW_TOPDIR:-$PROJECT_ROOT/rpmbuild-review}"
REVIEW_TMPDIR="$REVIEW_TOPDIR/TMP"
MOCK_RESULTDIR="$REVIEW_TOPDIR/mock-results"

require_command() {
  local command_name="$1"

  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "*** Missing required command: $command_name ***" >&2
    exit 1
  fi
}

run_step() {
  local title="$1"
  shift

  printf '\n*** %s ***\n' "$title"
  "$@"
}

require_command mock
require_command rpmbuild
require_command rpmlint
require_command rpmspec
require_command spectool

rm -rf "$REVIEW_TOPDIR"
mkdir -p \
  "$REVIEW_TOPDIR/BUILD" \
  "$REVIEW_TOPDIR/BUILDROOT" \
  "$REVIEW_TOPDIR/RPMS" \
  "$REVIEW_TOPDIR/SOURCES" \
  "$REVIEW_TOPDIR/SPECS" \
  "$REVIEW_TOPDIR/SRPMS" \
  "$REVIEW_TMPDIR" \
  "$MOCK_RESULTDIR"

run_step "Download upstream sources from spec" spectool -g -C "$REVIEW_TOPDIR/SOURCES" "$SPEC_FILE"
cp "$SPEC_FILE" "$REVIEW_TOPDIR/SPECS/"

run_step "Build source RPM from downloaded source" \
  rpmbuild -bs \
  --define "_topdir $REVIEW_TOPDIR" \
  --define "_tmppath $REVIEW_TMPDIR" \
  "$REVIEW_TOPDIR/SPECS/$(basename "$SPEC_FILE")"

mapfile -t SRPM_FILES < <(
  find "$REVIEW_TOPDIR/SRPMS" -type f -name "*.src.rpm" |
    sort
)

if [ "${#SRPM_FILES[@]}" -ne 1 ]; then
  echo "*** Expected one source RPM under $REVIEW_TOPDIR/SRPMS, found ${#SRPM_FILES[@]} ***" >&2
  exit 1
fi

SRPM="${SRPM_FILES[0]}"

run_step "Rebuild source RPM in mock" mock -r "$MOCK_CONFIG" --resultdir "$MOCK_RESULTDIR" --rebuild "$SRPM"

mapfile -t RPM_FILES < <(
  find "$MOCK_RESULTDIR" -type f \
    -name "*.rpm" \
    ! -name "*.src.rpm" \
    ! -name "*-debuginfo-*.rpm" \
    ! -name "*-debugsource-*.rpm" |
    sort
)

if [ "${#RPM_FILES[@]}" -eq 0 ]; then
  echo "*** No binary RPMs found under $MOCK_RESULTDIR ***" >&2
  exit 1
fi

run_step "Run rpmlint" rpmlint "$SRPM" "${RPM_FILES[@]}"

printf '\n*** Package review checks completed ***\n'
printf 'Source RPM: %s\n' "$SRPM"
printf 'Mock result directory: %s\n' "$MOCK_RESULTDIR"
