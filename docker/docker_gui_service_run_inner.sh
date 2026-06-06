#!/usr/bin/env bash
set -e

# Docker GUI run helper for manual testing.
# The GUI talks to dnf5daemon on the system bus in this prototype.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

export DNFUI_MESON_BUILD_ROOT="/tmp/dnfui-build"
BUILD_DIR="$("$PROJECT_ROOT/utils/meson_build.sh" build-dir)"
APP_BIN="$BUILD_DIR/src/dnfui"
STARTED_SYSTEM_BUS_PID=""

echo "*** Building app ***"
"$PROJECT_ROOT/utils/meson_build.sh" app
meson compile -C "$BUILD_DIR" dnf-ui-gmo

# Normal packages install translation catalogs into the system locale directory.
# The Docker GUI target runs the build tree binary, so copy built catalogs into
# the container locale directory before starting GTK.
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

echo "*** Starting system bus for dnf5daemon Docker GUI testing ***"
start_system_bus
trap stop_system_bus EXIT

echo "*** Checking dnf5daemon D-Bus activation ***"
gdbus introspect --system --dest org.rpm.dnf.v0 --object-path /org/rpm/dnf/v0 >/dev/null

echo "*** Starting DNF UI ***"
"$APP_BIN"
