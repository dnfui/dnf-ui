#!/usr/bin/env bash
set -e

# Docker GDB run helper that starts the app under gdbserver and starts the
# transaction service on the same session bus inside the container.

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck disable=SC1091
source "$PROJECT_ROOT/utils/transaction_service_paths.conf"

export DNFUI_MESON_BUILD_ROOT="/tmp/dnfui-build"
BUILD_DIR="$("$PROJECT_ROOT/utils/meson_build.sh" build-dir)"
SERVICE_BIN="$BUILD_DIR/src/service/$TRANSACTION_SERVICE_BIN_NAME"
APP_BIN="$BUILD_DIR/src/dnfui"
GDB_PORT="${GDB_PORT:-2345}"

echo "*** Building app and transaction service ***"
"$PROJECT_ROOT/utils/meson_build.sh" all

for path in "$SERVICE_BIN" "$APP_BIN"; do
  if [ ! -x "$path" ]; then
    echo "*** Missing runtime file: $path ***" >&2
    exit 1
  fi
done

echo "*** Starting session bus transaction service for Docker GDB debugging ***"

export DNFUI_TRANSACTION_BUS=session
export SERVICE_BIN
export TRANSACTION_SERVICE_NAME
export APP_BIN
export GDB_PORT

dbus-run-session -- bash <<'EOF'
set -e

# Start the transaction service on the session bus:
"$SERVICE_BIN" --session >/tmp/dnfui-service.log 2>&1 &

# Save the service process ID so it can be stopped when GDB exits:
service_pid=$!

# Stop the transaction service when GDB exits:
trap 'kill "$service_pid" >/dev/null 2>&1 || true; wait "$service_pid" >/dev/null 2>&1 || true' EXIT

# Wait until the transaction service has claimed its D-Bus name:
gdbus wait --session "$TRANSACTION_SERVICE_NAME" >/dev/null

# Start the UI under gdbserver in the same session bus environment:
exec gdbserver --once "0.0.0.0:$GDB_PORT" "$APP_BIN"
EOF
