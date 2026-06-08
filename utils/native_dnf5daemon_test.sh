#!/usr/bin/env bash
set -e

# Runs dnf5daemon client tests on the native Fedora system.
# The setup steps install and remove the selected test package so each test gets
# the package state it expects.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="$("$PROJECT_ROOT/utils/meson_build.sh" build-dir)"
TEST_BIN="$BUILD_DIR/test/dnfui-tests"
INSTALL_SPEC="${DNFUI_TEST_DNF5DAEMON_INSTALL_SPEC:-cowsay}"
INSTALL_NAME="${DNFUI_TEST_DNF5DAEMON_INSTALL_NAME:-$INSTALL_SPEC}"
RUN_APPLY_TESTS="${DNFUI_NATIVE_DNF5DAEMON_APPLY:-0}"
PACKAGE_WAS_INSTALLED="no"

remove_test_package() {
  sudo dnf5 -y remove "$INSTALL_SPEC" >/dev/null 2>&1 || true
}

install_test_package() {
  sudo dnf5 -y install "$INSTALL_SPEC" >/dev/null
  rpm -q "$INSTALL_NAME" >/dev/null
}

restore_test_package() {
  if [ "$PACKAGE_WAS_INSTALLED" = "yes" ]; then
    install_test_package
  else
    remove_test_package
  fi
}

run_daemon_test() {
  local test_name="$1"

  echo "*** Running native dnf5daemon test: $test_name ***"
  DNFUI_TEST_DNF5DAEMON=1 "$TEST_BIN" "$test_name"
}

echo "*** Building tests ***"
"$PROJECT_ROOT/utils/meson_build.sh" tests

if [ ! -x "$TEST_BIN" ]; then
  echo "*** Missing test binary: $TEST_BIN ***" >&2
  exit 1
fi

echo "*** Checking dnf5daemon D-Bus activation ***"
gdbus introspect --system --dest org.rpm.dnf.v0 --object-path /org/rpm/dnf/v0 >/dev/null

echo "*** Preparing package metadata ***"
sudo dnf5 makecache >/dev/null

if rpm -q "$INSTALL_NAME" >/dev/null 2>&1; then
  PACKAGE_WAS_INSTALLED="yes"
fi
trap restore_test_package EXIT

echo "*** Native dnf5daemon tests may install and remove $INSTALL_SPEC. ***"

remove_test_package
run_daemon_test "dnf5daemon client previews install requests"
remove_test_package

run_daemon_test "dnf5daemon client previews upgrade-all requests"
remove_test_package

run_daemon_test "dnf5daemon client releases preview sessions"
remove_test_package

if [ "$RUN_APPLY_TESTS" = "1" ]; then
  echo "*** Running native apply tests. These require dnf5daemon authorization. ***"

  run_daemon_test "dnf5daemon client applies install requests"
  rpm -q "$INSTALL_NAME" >/dev/null
  remove_test_package

  install_test_package
  run_daemon_test "dnf5daemon client releases sessions after failed apply"
  install_test_package
  run_daemon_test "dnf5daemon client previews remove requests"
  run_daemon_test "dnf5daemon client previews reinstall requests"
  remove_test_package
else
  echo "*** Skipping native apply tests. Set DNFUI_NATIVE_DNF5DAEMON_APPLY=1 to run them. ***"
fi

run_daemon_test "dnf5daemon client rejects removing dnf5daemon-server"
run_daemon_test "dnf5daemon client reports resolve failure"
run_daemon_test "dnf5daemon client reports unavailable daemon"
