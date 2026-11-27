#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="$1"
shift

# Meson uses coredata.dat to identify a configured build directory.
# Leave empty or incomplete directories to the normal setup command.
if [ ! -f "$BUILD_DIR/meson-private/coredata.dat" ]; then
  exit 0
fi

# Meson 1.11.2 validates command line options against the configured project
# before reading new project options. Register build_tui before setting it.
OPTIONS=$("$@" introspect --buildoptions "$BUILD_DIR")
REGISTERED=$(python3 -c '
import json
import sys

options = json.load(sys.stdin)
print(any(option["name"] == "build_tui" for option in options))
' <<< "$OPTIONS")
if [ "$REGISTERED" = "False" ]; then
  "$@" setup "$BUILD_DIR" --reconfigure
fi
