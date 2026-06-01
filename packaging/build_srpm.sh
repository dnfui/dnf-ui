#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SPEC_FILE="${SPEC_FILE:-$PROJECT_ROOT/dnf-ui.spec}"
RPM_TOPDIR="${RPM_TOPDIR:-$PROJECT_ROOT/rpmbuild}"
RPM_TMPDIR="${RPM_TMPDIR:-$RPM_TOPDIR/TMP}"

if [ ! -f "$SPEC_FILE" ]; then
  echo "*** Missing spec file: $SPEC_FILE ***" >&2
  exit 1
fi

PACKAGE_NAME="$(rpmspec -q --srpm --qf '%{NAME}\n' "$SPEC_FILE" | head -n 1)"
PACKAGE_VERSION="$(rpmspec -q --srpm --qf '%{VERSION}\n' "$SPEC_FILE" | head -n 1)"
SOURCE_TARBALL="$RPM_TOPDIR/SOURCES/${PACKAGE_NAME}-${PACKAGE_VERSION}.tar.gz"

mkdir -p \
  "$RPM_TOPDIR/BUILD" \
  "$RPM_TOPDIR/BUILDROOT" \
  "$RPM_TOPDIR/RPMS" \
  "$RPM_TOPDIR/SOURCES" \
  "$RPM_TOPDIR/SPECS" \
  "$RPM_TOPDIR/SRPMS" \
  "$RPM_TMPDIR"

# Package only tracked source files so ignored build outputs do not enter the
# release archive.
mapfile -d '' SOURCE_FILES < <(git -C "$PROJECT_ROOT" ls-files -z)

tar \
  --create \
  --gzip \
  --file "$SOURCE_TARBALL" \
  --directory "$PROJECT_ROOT" \
  --transform "s,^,${PACKAGE_NAME}-${PACKAGE_VERSION}/," \
  "${SOURCE_FILES[@]}"

cp "$SPEC_FILE" "$RPM_TOPDIR/SPECS/"

rpmbuild -bs \
  --define "_topdir $RPM_TOPDIR" \
  --define "_tmppath $RPM_TMPDIR" \
  "$RPM_TOPDIR/SPECS/$(basename "$SPEC_FILE")"

# Keep a stable symlink for scripts and CI steps that need the newest SRPM
# without knowing its exact release suffix.
LATEST_SRPM="$(
  find "$RPM_TOPDIR/SRPMS" -type f -name "*.src.rpm" -printf '%T@ %p\n' |
    sort -n |
    tail -1 |
    cut -d' ' -f2-
)"

test -n "$LATEST_SRPM"

ln -sfn "$LATEST_SRPM" "$PROJECT_ROOT/dnf-ui-latest.src.rpm"
