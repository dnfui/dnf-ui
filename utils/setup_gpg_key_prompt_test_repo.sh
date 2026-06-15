#!/usr/bin/env bash
set -euo pipefail

# Create or remove a local repo used to test DNF UI's repository key prompt.
# Use this only in a disposable Fedora VM.
#
# The repo has a valid package signing key in gpgkey, but that key is not
# trusted by rpm until the user accepts it.
# That is what makes dnf5daemon ask DNF UI whether the key should be trusted.

TEST_ROOT="/tmp/dnfui-gpg-test"
GNUPG_HOME="$TEST_ROOT/gnupg"
KEY_NAME="DNF UI test repo"
KEY_EMAIL="dnfui-test@example.invalid"
KEY_FILE="$TEST_ROOT/RPM-GPG-KEY-dnfui-test"
SYSTEM_KEY_FILE="/etc/pki/rpm-gpg/RPM-GPG-KEY-dnfui-test"
REPO_FILE="/etc/yum.repos.d/dnfui-gpg-test.repo"
PACKAGE_NAME="dnfui-gpg-test-package"
PACKAGE_VERSION="1"
RPMBUILD_ROOT="$HOME/rpmbuild"

usage() {
  echo "Usage: $0 setup|restore|status" >&2
}

# Run privileged commands through sudo when the script is not already root.
sudo_command() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
    return
  fi

  sudo "$@"
}

# Fail early with a package hint instead of breaking halfway through setup.
require_command() {
  local command_name="$1"
  local package_name="$2"

  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Missing command: $command_name" >&2
    echo "Install it with: sudo dnf install -y $package_name" >&2
    exit 1
  fi
}

# These tools are needed to build, sign, publish, and clean the local RPM repo.
check_dependencies() {
  require_command gpg gnupg2
  require_command rpmbuild rpm-build
  require_command rpmsign rpm-sign
  require_command createrepo_c createrepo_c
  require_command dnf dnf5
  require_command rpm rpm
}

# rpm stores trusted repository keys as gpg-pubkey packages.
# Find only the temporary key created by this script.
imported_test_keys() {
  if ! command -v rpm >/dev/null 2>&1; then
    return
  fi

  rpm -qa 'gpg-pubkey*' --qf '%{NAME}-%{VERSION}-%{RELEASE} %{SUMMARY}\n' | grep "$KEY_NAME" || true
}

# Remove the imported test key so the next install asks for trust again.
remove_imported_test_keys() {
  local key_packages=()

  while IFS= read -r line; do
    if [ -n "$line" ]; then
      key_packages+=("$(printf "%s" "$line" | awk '{print $1}')")
    fi
  done < <(imported_test_keys)

  if [ "${#key_packages[@]}" -eq 0 ]; then
    return
  fi

  sudo_command rpm -e "${key_packages[@]}"
}

# Put the machine back where it was before setup ran.
restore_repo() {
  sudo_command dnf remove -y "$PACKAGE_NAME" >/dev/null 2>&1 || true
  sudo_command rm -f "$REPO_FILE"
  sudo_command rm -f "$SYSTEM_KEY_FILE"
  remove_imported_test_keys
  sudo_command dnf clean all >/dev/null 2>&1 || true

  rm -rf "$TEST_ROOT"
  rm -f "$RPMBUILD_ROOT/SOURCES/$PACKAGE_NAME-$PACKAGE_VERSION.tar.gz"
  rm -f "$RPMBUILD_ROOT/SPECS/$PACKAGE_NAME.spec"
  rm -f "$RPMBUILD_ROOT/RPMS/noarch/$PACKAGE_NAME-$PACKAGE_VERSION"-*.noarch.rpm

  echo "Removed DNF UI GPG prompt test repo."
}

# Create a local signing key without a password.
# The key is valid, but rpm will not trust it until the user accepts it.
create_key() {
  mkdir -p "$GNUPG_HOME"
  chmod 700 "$GNUPG_HOME"

  cat >"$TEST_ROOT/key.batch" <<EOF
%no-protection
Key-Type: RSA
Key-Length: 2048
Name-Real: $KEY_NAME
Name-Email: $KEY_EMAIL
Expire-Date: 0
%commit
EOF

  GNUPGHOME="$GNUPG_HOME" gpg --batch --generate-key "$TEST_ROOT/key.batch"
  GNUPGHOME="$GNUPG_HOME" gpg --armor --export "$KEY_NAME" >"$KEY_FILE"
}

# Build a tiny noarch package so the test repo has something safe to install.
create_package_source() {
  mkdir -p "$RPMBUILD_ROOT/BUILD" "$RPMBUILD_ROOT/RPMS" "$RPMBUILD_ROOT/SOURCES" "$RPMBUILD_ROOT/SPECS" "$RPMBUILD_ROOT/SRPMS"
  mkdir -p "$TEST_ROOT/$PACKAGE_NAME-$PACKAGE_VERSION"
  echo "DNF UI GPG prompt test package" >"$TEST_ROOT/$PACKAGE_NAME-$PACKAGE_VERSION/README"
  tar -C "$TEST_ROOT" -czf "$RPMBUILD_ROOT/SOURCES/$PACKAGE_NAME-$PACKAGE_VERSION.tar.gz" "$PACKAGE_NAME-$PACKAGE_VERSION"

  cat >"$RPMBUILD_ROOT/SPECS/$PACKAGE_NAME.spec" <<EOF
Name:           $PACKAGE_NAME
Version:        $PACKAGE_VERSION
Release:        1%{?dist}
Summary:        Local package for DNF UI GPG prompt testing
License:        MIT
BuildArch:      noarch
Source0:        %{name}-%{version}.tar.gz

%description
Small local package used only to test DNF UI repository key prompts.

%prep
%autosetup

%build

%install
mkdir -p %{buildroot}%{_datadir}/dnfui-gpg-test
install -m 0644 README %{buildroot}%{_datadir}/dnfui-gpg-test/README

%files
%{_datadir}/dnfui-gpg-test/README
EOF
}

# Sign the package with the temporary key so dnf5daemon has to ask about it.
build_and_sign_package() {
  rpmbuild -bb "$RPMBUILD_ROOT/SPECS/$PACKAGE_NAME.spec"

  rpmsign --addsign \
    --define "_gpg_name $KEY_NAME <$KEY_EMAIL>" \
    --define "_gpg_path $GNUPG_HOME" \
    "$RPMBUILD_ROOT/RPMS/noarch/$PACKAGE_NAME-$PACKAGE_VERSION"-*.noarch.rpm
}

# Publish the signed RPM through a local file based repo.
# The repo advertises the key, but the key is not imported into rpm yet.
create_repo() {
  mkdir -p "$TEST_ROOT/repo"
  cp "$RPMBUILD_ROOT/RPMS/noarch/$PACKAGE_NAME-$PACKAGE_VERSION"-*.noarch.rpm "$TEST_ROOT/repo/"
  createrepo_c "$TEST_ROOT/repo"

  sudo_command install -m 0644 "$KEY_FILE" "$SYSTEM_KEY_FILE"

  sudo_command tee "$REPO_FILE" >/dev/null <<EOF
[dnfui-gpg-test]
name=DNF UI local GPG prompt test
baseurl=file://$TEST_ROOT/repo
enabled=1
gpgcheck=1
repo_gpgcheck=0
gpgkey=file://$SYSTEM_KEY_FILE
skip_if_unavailable=False
metadata_expire=0
EOF

  sudo_command dnf clean metadata >/dev/null
}

# Recreate the whole test setup from scratch so repeated manual tests are clean.
setup_repo() {
  check_dependencies
  restore_repo

  mkdir -p "$TEST_ROOT"
  create_key
  create_package_source
  build_and_sign_package
  create_repo

  echo
  echo "DNF UI GPG prompt test repo is ready."
  echo "Search for this package in DNF UI:"
  echo "  $PACKAGE_NAME"
  echo
  echo "Reject should fail the transaction."
  echo "Trust Key should import the key and install the package."
}

# Show whether the repo, package, and imported test key are present.
show_status() {
  if [ -f "$REPO_FILE" ]; then
    echo "Repo file exists: $REPO_FILE"
  else
    echo "Repo file missing: $REPO_FILE"
  fi

  if command -v rpm >/dev/null 2>&1 && rpm -q "$PACKAGE_NAME" >/dev/null 2>&1; then
    echo "Package installed: $PACKAGE_NAME"
  else
    echo "Package not installed: $PACKAGE_NAME"
  fi

  echo "Imported DNF UI test keys:"
  imported_test_keys
}

case "${1:-}" in
"setup")
  setup_repo
  ;;
"restore")
  restore_repo
  ;;
"status")
  show_status
  ;;
*)
  usage
  exit 1
  ;;
esac
