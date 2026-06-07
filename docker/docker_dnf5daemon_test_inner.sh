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
INSTALL_NAME="${DNFUI_TEST_DNF5DAEMON_INSTALL_NAME:-$INSTALL_SPEC}"

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

remove_test_package() {
  dnf5 -y remove "$INSTALL_SPEC" >/dev/null 2>&1 || true
}

install_test_package() {
  dnf5 -y install "$INSTALL_SPEC" >/dev/null 2>&1
  rpm -q "$INSTALL_NAME" >/dev/null
}

run_daemon_test() {
  local test_name="$1"

  echo "*** Running dnf5daemon test: $test_name ***"
  "$TEST_BIN" "$test_name"
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
remove_test_package

run_daemon_test "dnf5daemon client previews install requests"
remove_test_package

run_daemon_test "dnf5daemon client applies install requests"
rpm -q "$INSTALL_NAME" >/dev/null
remove_test_package

install_test_package
run_daemon_test "dnf5daemon client previews remove requests"
run_daemon_test "dnf5daemon client previews reinstall requests"
remove_test_package

run_daemon_test "dnf5daemon client reports resolve failure"
run_daemon_test "dnf5daemon client reports unavailable daemon"
