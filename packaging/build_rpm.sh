#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SPEC_FILE="${SPEC_FILE:-$PROJECT_ROOT/dnf-ui.spec}"
RPM_TOPDIR="${RPM_TOPDIR:-$PROJECT_ROOT/rpmbuild}"
RPM_TMPDIR="${RPM_TMPDIR:-$RPM_TOPDIR/TMP}"

PACKAGE_NAME="$(rpmspec -q --srpm --qf '%{NAME}\n' "$SPEC_FILE" | head -n 1)"

# Build the SRPM first so local RPM builds and release publishing use the same
# source archive path.
"$SCRIPT_DIR/build_srpm.sh"

rpmbuild -ba \
  --define "_topdir $RPM_TOPDIR" \
  --define "_tmppath $RPM_TMPDIR" \
  "$RPM_TOPDIR/SPECS/$(basename "$SPEC_FILE")"

# Point the convenience symlink at the newest installable RPM, not the debug
# packages produced by the same build.
LATEST_RPM="$(
  find "$RPM_TOPDIR/RPMS" -type f \
    -name "${PACKAGE_NAME}-*.rpm" \
    ! -name "${PACKAGE_NAME}-debuginfo-*.rpm" \
    ! -name "${PACKAGE_NAME}-debugsource-*.rpm" \
    -printf '%T@ %p\n' |
    sort -n |
    tail -1 |
    cut -d' ' -f2-
)"

test -n "$LATEST_RPM"

ln -sfn "$LATEST_RPM" "$PROJECT_ROOT/dnf-ui-latest.rpm"
