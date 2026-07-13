#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SPEC_FILE="${SPEC_FILE:-$PROJECT_ROOT/dnf-ui.spec}"
MOCK_CONFIG="${MOCK_CONFIG:-fedora-rawhide-x86_64}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$PROJECT_ROOT/rpmbuild/SOURCES}"

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

require_command make
require_command mock
require_command rpmlint
require_command rpmspec
require_command spectool

mkdir -p "$DOWNLOAD_DIR"

run_step "Download upstream sources from spec" spectool -g -C "$DOWNLOAD_DIR" "$SPEC_FILE"
run_step "Build source and binary RPMs" make -C "$PROJECT_ROOT" rpm

SRPM="$PROJECT_ROOT/dnf-ui-latest.src.rpm"
if [ ! -e "$SRPM" ]; then
  echo "*** Missing source RPM: $SRPM ***" >&2
  exit 1
fi
SRPM="$(readlink -f "$SRPM")"

mapfile -t RPM_FILES < <(
  find "$PROJECT_ROOT/rpmbuild/RPMS" -type f \
    -name "*.rpm" \
    ! -name "*-debuginfo-*.rpm" \
    ! -name "*-debugsource-*.rpm" |
    sort
)

if [ "${#RPM_FILES[@]}" -eq 0 ]; then
  echo "*** No binary RPMs found under $PROJECT_ROOT/rpmbuild/RPMS ***" >&2
  exit 1
fi

run_step "Run rpmlint" rpmlint "$SRPM" "${RPM_FILES[@]}"
run_step "Rebuild source RPM in mock" mock -r "$MOCK_CONFIG" --rebuild "$SRPM"

printf '\n*** Package review checks completed ***\n'
