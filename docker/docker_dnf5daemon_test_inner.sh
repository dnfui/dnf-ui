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
REPOSITORY_CLIENT_TEST_REPO_ID="${DNFUI_TEST_REPOSITORY_CLIENT_REPO_ID:-dnfui-repository-client-test}"
REPOSITORY_CLIENT_TEST_REPO_FILE="/etc/yum.repos.d/${REPOSITORY_CLIENT_TEST_REPO_ID}.repo"
API_TEST_PACKAGE="dnfui-daemon-api-test"
API_TEST_OLD_VERSION="1.0"
API_TEST_NEW_VERSION="2.0"
API_TEST_REPO_FILE="/etc/yum.repos.d/$API_TEST_PACKAGE.repo"
API_TEST_ROOT=""

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

setup_repository_client_test_repo() {
  mkdir -p /tmp/dnfui-repository-client-test
  cat >"$REPOSITORY_CLIENT_TEST_REPO_FILE" <<EOF
[$REPOSITORY_CLIENT_TEST_REPO_ID]
name=DNF UI Repository Client Test
baseurl=file:///tmp/dnfui-repository-client-test
enabled=0
gpgcheck=0
EOF
}

remove_repository_client_test_repo() {
  rm -f "$REPOSITORY_CLIENT_TEST_REPO_FILE"
}

build_api_test_package() {
  local version="$1"
  local spec_file="$API_TEST_ROOT/rpmbuild/SPECS/$API_TEST_PACKAGE.spec"

  cat >"$spec_file" <<EOF
Name:           $API_TEST_PACKAGE
Version:        $version
Release:        1
Summary:        Local package for dnf5daemon API testing
License:        MIT
BuildArch:      noarch

%description
Small local package used to test daemon upgrade and downgrade previews.

%prep

%build

%install
mkdir -p %{buildroot}%{_datadir}/$API_TEST_PACKAGE
printf '%s\n' '%{version}' > %{buildroot}%{_datadir}/$API_TEST_PACKAGE/version

%files
%{_datadir}/$API_TEST_PACKAGE/version
EOF

  if ! rpmbuild -bb --define "_topdir $API_TEST_ROOT/rpmbuild" "$spec_file" \
      >"$API_TEST_ROOT/rpmbuild-$version.log" 2>&1; then
    cat "$API_TEST_ROOT/rpmbuild-$version.log" >&2
    return 1
  fi
}

setup_api_test_repo() {
  API_TEST_ROOT="$(mktemp -d /tmp/dnfui-daemon-api-test.XXXXXX)"
  mkdir -p "$API_TEST_ROOT/rpmbuild/BUILD" "$API_TEST_ROOT/rpmbuild/BUILDROOT" \
    "$API_TEST_ROOT/rpmbuild/RPMS" "$API_TEST_ROOT/rpmbuild/SOURCES" \
    "$API_TEST_ROOT/rpmbuild/SPECS" "$API_TEST_ROOT/rpmbuild/SRPMS" "$API_TEST_ROOT/repo"

  build_api_test_package "$API_TEST_OLD_VERSION"
  build_api_test_package "$API_TEST_NEW_VERSION"
  cp "$API_TEST_ROOT/rpmbuild/RPMS/noarch/"*.rpm "$API_TEST_ROOT/repo/"
  createrepo_c "$API_TEST_ROOT/repo" >/dev/null

  cat >"$API_TEST_REPO_FILE" <<EOF
[$API_TEST_PACKAGE]
name=DNF UI daemon API test
baseurl=file://$API_TEST_ROOT/repo
enabled=1
gpgcheck=0
metadata_expire=0
EOF
}

cleanup_api_test_repo() {
  if [ -n "$API_TEST_ROOT" ]; then
    rm -f "$API_TEST_REPO_FILE"
    rm -rf "$API_TEST_ROOT"
    API_TEST_ROOT=""
  fi
}

run_daemon_test() {
  local test_name="$1"

  echo "*** Running dnf5daemon test: $test_name ***"
  "$TEST_BIN" "$test_name"
}

# Reinstall tests need the installed daemon version to remain available in the repositories.
# Update it before activation so the daemon starts with the matching installed libraries.
dnf5 -y upgrade dnf5daemon-server >/dev/null

echo "*** Building tests ***"
"$PROJECT_ROOT/utils/meson_build.sh" tests

if [ ! -x "$TEST_BIN" ]; then
  echo "*** Missing test binary: $TEST_BIN ***" >&2
  exit 1
fi

echo "*** Starting system bus for dnf5daemon tests ***"
start_system_bus
trap 'cleanup_api_test_repo; remove_repository_client_test_repo; stop_system_bus' EXIT

echo "*** Checking dnf5daemon D-Bus activation ***"
gdbus introspect --system --dest org.rpm.dnf.v0 --object-path /org/rpm/dnf/v0 >/dev/null

echo "*** Preparing package metadata ***"
dnf5 makecache >/dev/null
remove_test_package
setup_repository_client_test_repo

run_daemon_test "dnf5daemon client previews install requests"
remove_test_package

run_daemon_test "dnf5daemon client previews upgrade-all requests"
remove_test_package

if [ -n "${DNFUI_TEST_DNF5DAEMON_DOWNGRADE_SPEC:-}" ]; then
  run_daemon_test "dnf5daemon client previews downgrade requests"
  remove_test_package
fi

run_daemon_test "dnf5daemon client lists upgrade targets"
remove_test_package

run_daemon_test "dnf5daemon client refreshes repositories"
remove_test_package

DNFUI_TEST_REPOSITORY_CLIENT=1 run_daemon_test "Repository client applies changes through fresh daemon sessions"
remove_test_package

setup_api_test_repo
dnf5 makecache >/dev/null
dnf5 -y install "$API_TEST_ROOT/repo/$API_TEST_PACKAGE-$API_TEST_OLD_VERSION-1.noarch.rpm" >/dev/null
test "$(rpm -q --qf '%{VERSION}' "$API_TEST_PACKAGE")" = "$API_TEST_OLD_VERSION"
DNFUI_TEST_DNF5DAEMON_UPGRADE_SPEC="$API_TEST_PACKAGE.noarch" \
  DNFUI_TEST_DNF5DAEMON_UPGRADE_EXPECTED_NEVRA="$API_TEST_PACKAGE-$API_TEST_NEW_VERSION-1.noarch" \
  run_daemon_test "dnf5daemon client previews selected upgrade requests"

dnf5 -y upgrade "$API_TEST_PACKAGE" >/dev/null
test "$(rpm -q --qf '%{VERSION}' "$API_TEST_PACKAGE")" = "$API_TEST_NEW_VERSION"
DNFUI_TEST_DNF5DAEMON_DOWNGRADE_SPEC="$API_TEST_PACKAGE-$API_TEST_OLD_VERSION-1.noarch" \
  run_daemon_test "dnf5daemon client previews downgrade requests"
dnf5 -y remove "$API_TEST_PACKAGE" >/dev/null
cleanup_api_test_repo

run_daemon_test "dnf5daemon client releases preview sessions"
remove_test_package

run_daemon_test "dnf5daemon client applies install requests"
rpm -q "$INSTALL_NAME" >/dev/null
remove_test_package

install_test_package
run_daemon_test "dnf5daemon client releases sessions after failed apply"
install_test_package
run_daemon_test "dnf5daemon client previews remove requests"
run_daemon_test "dnf5daemon client previews reinstall requests"
remove_test_package

DNFUI_TEST_DNF5DAEMON_OFFLINE=1 run_daemon_test "dnf5daemon client prepares daemon changes for reboot"
DNFUI_TEST_DNF5DAEMON_OFFLINE=1 run_daemon_test "dnf5daemon client discards prepared updates"
DNFUI_TEST_DNF5DAEMON_OFFLINE=1 run_daemon_test "dnf5daemon client discards unreadable offline updates"
DNFUI_TEST_DNF5DAEMON_OFFLINE=1 run_daemon_test "dnf5daemon client prepares daemon changes for reboot"
dnf5 offline status
dnf5 offline clean
run_daemon_test "dnf5daemon client previews install requests"

run_daemon_test "dnf5daemon client rejects removing dnf5daemon-server"
run_daemon_test "dnf5daemon client reports resolve failure"
run_daemon_test "dnf5daemon client reports unavailable daemon"
