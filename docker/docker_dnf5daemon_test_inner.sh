#!/usr/bin/env bash
set -e

# Runs dnf5daemon client tests inside the development container.
# The tests use the public transaction client and a private system bus.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

export DNFUI_MESON_BUILD_ROOT="${DNFUI_MESON_BUILD_ROOT:-/tmp/dnfui-build}"
BUILD_DIR="$("$PROJECT_ROOT/utils/meson_build.sh" build-dir)"
TEST_BIN="$BUILD_DIR/test/dnfui-tests"
STARTED_SYSTEM_BUS_PID=""
INSTALL_SPEC="${DNFUI_TEST_DNF5DAEMON_INSTALL_SPEC:-cowsay}"

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

echo "*** Building tests ***"
"$PROJECT_ROOT/utils/meson_build.sh" tests

if [ ! -x "$TEST_BIN" ]; then
  echo "*** Missing test binary: $TEST_BIN ***" >&2
  exit 1
fi

echo "*** Starting system bus for dnf5daemon tests ***"
start_system_bus
trap stop_system_bus EXIT

echo "*** Checking dnf5daemon D-Bus activation ***"
gdbus introspect --system --dest org.rpm.dnf.v0 --object-path /org/rpm/dnf/v0 >/dev/null

echo "*** Preparing package metadata ***"
dnf5 makecache >/dev/null
dnf5 -y remove "$INSTALL_SPEC" >/dev/null 2>&1 || true

echo "*** Running dnf5daemon tests ***"
"$TEST_BIN" "[dnf5daemon]"
