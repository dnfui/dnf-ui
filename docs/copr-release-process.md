# DNF UI COPR release process

This document describes how to build, test, publish, and update the `dnf-ui` COPR package.

COPR project:

```text
https://copr.fedorainfracloud.org/coprs/erikmn/dnf-ui/
```

## 1. Required local tools

Install COPR CLI:

```bash
sudo dnf install copr-cli
```

Install the native build dependencies listed by the project:

```bash
sudo dnf install $(grep -vE '^\s*(#|$)' docs/fedora-native-dependencies.txt)
```

The dependency file covers native builds, tests, package validation, RPM builds,
translations, mock builds, and dnf5daemon based app testing.

## 2. Mock setup

If `mock` says the current user is not in the `mock` group, add the user:

```bash
sudo usermod -a -G mock "$USER"
```

Then log out and log back in.

Verify that the group is active:

```bash
groups
```

The output should include:

```text
mock
```

Then `mock` can be used as a normal user:

```bash
mock -r fedora-<release>-x86_64 --rebuild dnf-ui-latest.src.rpm
```

Do **not** add untrusted users to the `mock` group. Mock group membership is powerful.

## 3. Build the SRPM

The repository already has packaging helpers:

```bash
./packaging/build_srpm.sh
./packaging/build_rpm.sh
```

The SRPM helper creates a source tarball from tracked Git files, builds the SRPM, and updates this convenience symlink:

```text
dnf-ui-latest.src.rpm
```

Important: the SRPM helper uses Git tracked files. Before building a release, check for untracked or uncommitted files:

```bash
git status --short
```

Commit anything that must be included in the COPR build.

Build the SRPM:

```bash
./packaging/build_srpm.sh
```

Show the real SRPM path:

```bash
readlink -f dnf-ui-latest.src.rpm
```

Optional local binary RPM build:

```bash
./packaging/build_rpm.sh
```

This also updates:

```text
dnf-ui-latest.rpm
```

## 4. Test the SRPM locally before uploading

Run `rpmlint`:

```bash
rpmlint dnf-ui.spec dnf-ui-latest.src.rpm
```

Rebuild the SRPM in clean Fedora build roots:

```bash
mock -r fedora-<current>-x86_64 --rebuild dnf-ui-latest.src.rpm
mock -r fedora-<previous>-x86_64 --rebuild dnf-ui-latest.src.rpm
```

Only upload to COPR after these builds pass.

## 5. COPR CLI setup

Get the COPR CLI token from:

```text
https://copr.fedorainfracloud.org/api/
```

Create the config directory if needed:

```bash
mkdir -p ~/.config
chmod 700 ~/.config
```

Paste the generated COPR config block into:

```text
~/.config/copr
```

Secure the file:

```bash
chmod 600 ~/.config/copr
```

Verify the COPR account:

```bash
copr-cli whoami
```

For this project it should print:

```text
erikmn
```

The COPR API token expires after 180 days. If COPR CLI authentication starts failing later, renew the token from the same API page.

### GitHub Actions COPR publishing

The repository has a GitHub Actions workflow that publishes tagged releases to COPR:

```text
.github/workflows/publish-copr.yml
```

It runs when a tag like `0.1.0` is pushed. The workflow validates the tag,
checks that `dnf-ui.spec` has the same version, builds the SRPM, and submits it to COPR.

Set these GitHub repository secrets before using it:

```text
COPR_LOGIN
COPR_USERNAME
COPR_TOKEN
```

Do not commit the generated COPR config file or token.
The workflow writes the temporary COPR config inside the CI container from GitHub Secrets.

## 6. Create the COPR project

This was already done for `dnf-ui`, but these are the commands used:

```bash
copr-cli create \
  --chroot fedora-<current>-x86_64 \
  --chroot fedora-<previous>-x86_64 \
  dnf-ui
```

The created project is:

```text
https://copr.fedorainfracloud.org/coprs/erikmn/dnf-ui/
```

For normal public builds, the current and previous supported Fedora releases are enough. Rawhide can be added later if early breakage detection is useful.

## 7. Upload a build to COPR

From the repository root:

```bash
copr-cli build dnf-ui dnf-ui-latest.src.rpm
```

COPR will upload the SRPM and create a build.

Example build states:

```text
pending
importing
running
succeeded
```

Meaning:

```text
pending    COPR queued the build
importing  COPR imported the uploaded SRPM
running    COPR is building for the enabled Fedora chroots
succeeded  The build passed
```

It is safe to stop watching with `Ctrl-C`. That only stops the local terminal from watching the build. It does not cancel the COPR build.

To monitor a build later, use the build id printed by COPR:

```bash
copr-cli monitor xxxxxxxx
```

Or open the build URL printed by COPR.

## 8. Test install from COPR

If a native development install exists, remove it first so the RPM package is tested cleanly:

```bash
sudo make uninstall
sudo dnf remove dnf-ui
```

Enable the COPR and install the package using the commands from
[User install instructions](#9-user-install-instructions).

Then run the app:

```bash
dnfui
```

Smoke checks:

```bash
rpm -q dnf-ui
rpm -ql dnf-ui
rpm -q dnf5daemon-server
```

The app should trigger package actions through dnf5daemon, and Polkit
authorization should appear when a transaction is applied.

## 9. User install instructions

Use this in the README and release notes:

```bash
sudo dnf install dnf5-plugins
sudo dnf copr enable erikmn/dnf-ui
sudo dnf install dnf-ui
```

Run it with:

```bash
dnfui
```

Shorter version for announcements:

```bash
sudo dnf copr enable erikmn/dnf-ui
sudo dnf install dnf-ui
```

Users who already enabled the COPR do not need to enable it again for future versions.

## 10. Releasing a new app version

For a normal new version, update `dnf-ui.spec`.

Example:

```spec
Version:        <new-version>
Release:        1%{?dist}
```

Add a new changelog entry at the top of the changelog:

```spec
%changelog
* <rpm-date> ErikMN <erik@example.invalid> - <new-version>-1
- Short description of what changed
```

Then build, test, and upload:

```bash
./packaging/build_srpm.sh

mock -r fedora-<current>-x86_64 --rebuild dnf-ui-latest.src.rpm
mock -r fedora-<previous>-x86_64 --rebuild dnf-ui-latest.src.rpm

copr-cli build dnf-ui dnf-ui-latest.src.rpm
```

Suggested Git flow:

```bash
git add dnf-ui.spec
git commit -m "Release dnf-ui <new-version>"
git tag -a <new-version> -m "Release <new-version>"
git push
git push --tags
```

The GitHub release RPM workflow runs for tags that look like:

```text
<new-version>
```

The spec version should match the tag version.

## 11. Packaging rebuild without changing app version

If the app version is unchanged but the RPM packaging changes, only bump the `Release` field.

Example:

```spec
Version:        <current-version>
Release:        2%{?dist}
```

Add a changelog entry:

```spec
%changelog
* <rpm-date> ErikMN <erik@example.invalid> - <current-version>-2
- Fix packaging issue
```

Then build, test, and upload:

```bash
./packaging/build_srpm.sh

mock -r fedora-<current>-x86_64 --rebuild dnf-ui-latest.src.rpm
mock -r fedora-<previous>-x86_64 --rebuild dnf-ui-latest.src.rpm

copr-cli build dnf-ui dnf-ui-latest.src.rpm
```

## 12. Versioning rule

Do **not** upload a new public build with the same effective RPM version and release.

Bad repeated build name:

```text
dnf-ui-<version>-1
```

Good sequence:

```text
dnf-ui-<version>-1
dnf-ui-<version>-2
dnf-ui-<next-version>-1
dnf-ui-<next-next-version>-1
```

DNF decides whether something is an update from the RPM epoch, version, and release.
Since this package does not use an epoch, always move version or release forward for every public COPR upload.

## 13. What users do when there is a new version

Users who already enabled the COPR update normally:

```bash
sudo dnf upgrade dnf-ui
```

Or as part of a full system update:

```bash
sudo dnf upgrade
```

They do not need to run this again:

`sudo dnf copr enable erikmn/dnf-ui`

## 14. Useful local cleanup commands

Remove generated build output and RPM artifacts if needed:

```bash
make clean
make distclean
```

Remove a local development install before testing the RPM package:

```bash
sudo make uninstall
```

Remove the installed RPM package:

```bash
sudo dnf remove dnf-ui
```

## 15. References

COPR project:

```text
https://copr.fedorainfracloud.org/coprs/erikmn/dnf-ui/
```

COPR API token page:

```text
https://copr.fedorainfracloud.org/api/
```

COPR CLI documentation:

```text
https://developer.fedoraproject.org/deployment/copr/copr-cli.html
```

COPR user documentation:

```text
https://docs.pagure.org/copr.copr/user_documentation.html
```

Mock setup documentation:

```text
https://rpm-software-management.github.io/mock/#setup
```
