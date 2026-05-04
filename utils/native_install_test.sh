#!/usr/bin/env bash
# Build and reinstall the app as a native Fedora RPM test.
# This uses the existing project RPM build target.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SPEC_FILE="${SPEC_FILE:-$PROJECT_ROOT/dnf-ui.spec}"
LATEST_RPM="$PROJECT_ROOT/dnf-ui-latest.rpm"

# Read the package name from the RPM spec.
PACKAGE_NAME="$(rpmspec -q --srpm --qf '%{NAME}\n' "$SPEC_FILE" | head -n 1)"

cd "$PROJECT_ROOT"

# Remove the installed package first.
# A missing package is not an error.
if rpm -q "$PACKAGE_NAME" >/dev/null 2>&1; then
  sudo dnf remove -y "$PACKAGE_NAME"
else
  echo "*** $PACKAGE_NAME is not installed, skipping uninstall ***"
fi

# Build a fresh RPM with the normal project target.
make distclean && make rpm

# Install the latest RPM produced by the build.
sudo dnf install -y "$LATEST_RPM"

echo "*** Installed $LATEST_RPM ***"
