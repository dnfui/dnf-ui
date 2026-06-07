#!/usr/bin/env bash
set -e

# Build the app in the container, start dnf5daemon, and run gdbserver.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

export DNFUI_MESON_BUILD_ROOT="/tmp/dnfui-build"
BUILD_DIR="$("$PROJECT_ROOT/utils/meson_build.sh" build-dir)"
APP_BIN="$BUILD_DIR/src/dnfui"
STARTED_SYSTEM_BUS_PID=""

echo "*** Building app for GDB ***"
"$PROJECT_ROOT/utils/meson_build.sh" app
meson compile -C "$BUILD_DIR" dnf-ui-gmo

find "$BUILD_DIR/po" -path "*/LC_MESSAGES/dnf-ui.mo" -print0 | while IFS= read -r -d '' mo_file; do
  locale_dir="$(basename "$(dirname "$(dirname "$mo_file")")")"
  install -Dm0644 "$mo_file" "/usr/share/locale/$locale_dir/LC_MESSAGES/dnf-ui.mo"
done

if [ ! -x "$APP_BIN" ]; then
  echo "*** Missing runtime file: $APP_BIN ***" >&2
  exit 1
fi

start_system_bus() {
  mkdir -p /run/dbus

  if [ -S /run/dbus/system_bus_socket ]; then
    return
  fi

  rm -f /run/dbus/pid
  STARTED_SYSTEM_BUS_PID="$(dbus-daemon --system --fork --print-pid=1)"
}

stop_system_bus() {
  if [ -n "$STARTED_SYSTEM_BUS_PID" ]; then
    kill "$STARTED_SYSTEM_BUS_PID" >/dev/null 2>&1 || true
    wait "$STARTED_SYSTEM_BUS_PID" >/dev/null 2>&1 || true
  fi
}

echo "*** Starting system bus for dnf5daemon Docker GDB testing ***"
start_system_bus
trap stop_system_bus EXIT

echo "*** Checking dnf5daemon D-Bus activation ***"
gdbus introspect --system --dest org.rpm.dnf.v0 --object-path /org/rpm/dnf/v0 >/dev/null

echo "*** Starting GDB server on port 2345 ***"
gdbserver :2345 "$APP_BIN"
