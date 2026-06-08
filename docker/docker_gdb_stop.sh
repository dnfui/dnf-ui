#!/usr/bin/env bash
set -e

# Stop the Docker GDB container if it is still running.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/container_runtime.sh"

"$CONTAINER_RUNTIME" stop dnfui-gdb >/dev/null 2>&1 || true
