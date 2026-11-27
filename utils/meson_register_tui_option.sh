#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="$1"
shift

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
