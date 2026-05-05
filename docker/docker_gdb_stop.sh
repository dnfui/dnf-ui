#!/usr/bin/env bash
set -e

# Docker GDB stop helper that removes the running GDB container if it exists.

CONTAINER_NAME="${CONTAINER_NAME:-dnfui-gdb}"

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/container_runtime.sh"

if "$CONTAINER_RUNTIME" container inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
  echo "*** Stopping $CONTAINER_RUNTIME GDB container..."
  "$CONTAINER_RUNTIME" rm -f "$CONTAINER_NAME" >/dev/null
fi
