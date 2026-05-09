#!/usr/bin/env bash
set -euo pipefail

# Install Fedora packages needed for native DNF UI development.
# The package list is kept in a text file for review before running the script.
# Empty lines and lines starting with # are ignored.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPENDENCY_FILE="$PROJECT_ROOT/docs/fedora-native-dependencies.txt"

if [ ! -f "$DEPENDENCY_FILE" ]; then
  echo "Missing dependency file: $DEPENDENCY_FILE" >&2
  exit 1
fi

if ! command -v dnf >/dev/null 2>&1; then
  echo "This script requires Fedora dnf." >&2
  exit 1
fi

packages=()
while IFS= read -r line; do
  line="${line%%#*}"
  line="$(printf "%s" "$line" | xargs)"

  if [ -n "$line" ]; then
    packages+=("$line")
  fi
done <"$DEPENDENCY_FILE"

if [ "${#packages[@]}" -eq 0 ]; then
  echo "No packages found in $DEPENDENCY_FILE" >&2
  exit 1
fi

dnf_command=(dnf)
if [ "$(id -u)" -ne 0 ]; then
  if ! command -v sudo >/dev/null 2>&1; then
    echo "Run this script as root or install sudo." >&2
    exit 1
  fi

  dnf_command=(sudo dnf)
fi

echo "Installing Fedora development dependencies from:"
echo "  $DEPENDENCY_FILE"
echo

"${dnf_command[@]}" install -y "${packages[@]}"
