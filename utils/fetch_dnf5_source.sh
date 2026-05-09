#!/usr/bin/env bash
set -euo pipefail

# Clone or update the upstream dnf5 source tree for local reference.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

DNF5_REPO_URL="${DNF5_REPO_URL:-https://github.com/rpm-software-management/dnf5.git}"
DNF5_SOURCE_DIR="${DNF5_SOURCE_DIR:-$PROJECT_ROOT/external/dnf5}"

if [ -e "$DNF5_SOURCE_DIR" ] && [ ! -d "$DNF5_SOURCE_DIR/.git" ]; then
  echo "*** $DNF5_SOURCE_DIR exists but is not a git checkout ***" >&2
  exit 1
fi

if [ -d "$DNF5_SOURCE_DIR/.git" ]; then
  echo "*** Updating dnf5 source checkout in $DNF5_SOURCE_DIR ***"
  git -C "$DNF5_SOURCE_DIR" fetch --all --tags --prune
else
  echo "*** Cloning dnf5 source into $DNF5_SOURCE_DIR ***"
  mkdir -p "$(dirname "$DNF5_SOURCE_DIR")"
  git clone "$DNF5_REPO_URL" "$DNF5_SOURCE_DIR"
fi

echo "*** dnf5 source is available at $DNF5_SOURCE_DIR ***"
