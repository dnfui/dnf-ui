#!/usr/bin/env bash
set -e

# Docker GDB run helper that starts the app under gdbserver and starts the
# transaction service on a private session bus inside the container.

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
SESSION_BUS_ADDRESS="unix:path=/tmp/dnfui-gdb-session-bus"

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
export DBUS_SESSION_BUS_ADDRESS="$SESSION_BUS_ADDRESS"
export SERVICE_BIN
export TRANSACTION_SERVICE_NAME
export APP_BIN
export GDB_PORT

rm -f /tmp/dnfui-gdb-session-bus

# The host wrapper passes the same fixed socket address into the container.
# Starting the bus here gives both the app and service a private D-Bus endpoint.
dbus_info="$(dbus-daemon --session --fork --nopidfile --print-address=1 --print-pid=1 --address="$DBUS_SESSION_BUS_ADDRESS")"
DBUS_SESSION_BUS_ADDRESS="$(printf "%s\n" "$dbus_info" | sed -n '1p')"
dbus_pid="$(printf "%s\n" "$dbus_info" | sed -n '2p')"
export DBUS_SESSION_BUS_ADDRESS

# Start the transaction service on the private session bus:
"$SERVICE_BIN" --session >/tmp/dnfui-service.log 2>&1 &
service_pid=$!

cleanup() {
  kill "$service_pid" >/dev/null 2>&1 || true
  wait "$service_pid" >/dev/null 2>&1 || true
  kill "$dbus_pid" >/dev/null 2>&1 || true
  wait "$dbus_pid" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# Wait until the transaction service has claimed its D-Bus name before starting
# the UI. Otherwise the first preview request can race service startup.
gdbus wait --session "$TRANSACTION_SERVICE_NAME" >/dev/null

# Start the UI under gdbserver in the same session bus environment:
gdbserver --once "0.0.0.0:$GDB_PORT" "$APP_BIN"
