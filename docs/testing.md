# Testing

This document explains the test layout and what each group protects.

For the external API behavior these tests help protect, see
[External API assumptions](api-assumptions.md).

## Test types

The project uses:

- Catch2 tests under [test/unit](../test/unit)
- Docker helpers under [docker](../docker)

The Catch2 tests are the fastest place to check backend and client behavior.
New dnf5daemon-focused tests still need to be added on this prototype branch.

## Test dependencies

The test build needs:

- `pkgconfig(gio-2.0)`
- `pkgconfig(catch2-with-main)`

On Fedora, `catch-devel` provides `pkgconfig(catch2-with-main)`.

## Catch2 tests

Key files:

- [test/unit/test_backend.cpp](../test/unit/test_backend.cpp)
- [test/unit/test_config.cpp](../test/unit/test_config.cpp)
- [test/unit/test_package_action_rows.cpp](../test/unit/test_package_action_rows.cpp)
- [test/unit/test_package_query_cache.cpp](../test/unit/test_package_query_cache.cpp)
- [test/unit/test_pending_transaction_request.cpp](../test/unit/test_pending_transaction_request.cpp)
- [test/unit/test_search.cpp](../test/unit/test_search.cpp)
- [test/unit/test_transaction_preview.cpp](../test/unit/test_transaction_preview.cpp)
- [test/unit/test_transaction_request.cpp](../test/unit/test_transaction_request.cpp)
- [test/unit/test_offline.cpp](../test/unit/test_offline.cpp)

These tests protect:

- package search and merge behavior
- config file parsing and fallback behavior
- installed snapshot behavior
- package action row selection for install, upgrade, remove, and reinstall
- transaction preview behavior, including fail-closed preview building
- transaction request validation
- offline and cached metadata behavior

## Daemon smoke tests

The app now talks to Fedora dnf5daemon for privileged transactions.
Daemon-focused smoke tests still need to be added on this prototype branch.

## Common commands

Run the native test suite:

```sh
make test
```

Run the offline cached metadata smoke tests separately:

```sh
make dockerofflinetest
```

Run the Docker app target with networking disabled:

```sh
DOCKER_NETWORK_MODE=none make dockerrun
```

Run the Docker app target with the Swedish translation:

```sh
LANG=sv_SE.UTF-8 LANGUAGE=sv make dockerrun
CONTAINER_RUNTIME=podman LANG=sv_SE.UTF-8 LANGUAGE=sv make dockerrun
```

The Makefile is a task runner. Meson owns build configuration and test
definitions.

## Memory checks

Run a quick smoke test under Valgrind Memcheck:

```sh
make memcheck
```

The full automated Memcheck targets are disabled on the dnf5daemon prototype
branch until the old service tests are replaced.

Run the desktop app under Valgrind Memcheck:

```sh
make memcheck-app
```

Memcheck logs are written under `build/memcheck/`.

The default Memcheck setup fails on definite and indirect leaks from this
project. Reachable and possible leak noise from GLib, GTK, DNF, and related
libraries is suppressed in `utils/valgrind-dnfui.supp`.

Useful options:

```sh
MEMCHECK_SMOKE_FILTER="Transaction request validation rejects an empty request" make memcheck
MEMCHECK_SMOKE_TIMEOUT=5m make memcheck
MEMCHECK_SMOKE_TIMEOUT=10m make memcheck
```

## Docker notes

- `make dockerrun` starts a system bus in the container and uses dnf5daemon
- Use native Fedora to test the real desktop Polkit prompt

## Fedora review checks

Fedora package review expects `rpmlint` output for the source RPM and all binary
RPMs produced by the build. It is also normal to check that the source RPM builds
in `mock`.

The old Makefile RPM targets are disabled on the dnf5daemon prototype branch
until the package layout is checked for the daemon route.

After packaging is checked, build RPMs with the current packaging script or a
replacement Make target and run `rpmlint`:

```sh
rpmlint dnf-ui-latest.src.rpm rpmbuild/RPMS/*/*.rpm
```

Keep the `rpmlint` output so it can be included in the Fedora package review.

Build the source RPM in a clean Fedora build environment after the source RPM
build command has been restored:

```sh
mock -r fedora-rawhide-x86_64 --rebuild dnf-ui-latest.src.rpm
```

Use the Fedora release and architecture that match the package review you are
preparing. Running `mock` may require the user to be a member of the `mock`
group.

Reference: <https://docs.fedoraproject.org/en-US/packaging-guidelines/ReviewGuidelines/>

## What to test after changes

For documentation-only changes, run `git diff --check`.

For comments inside C++ source files, also run a target that compiles the changed
files.

For backend query changes, run the relevant Catch2 tests once the dnf5daemon
prototype test suite has been updated.

For transaction changes, test the dnf5daemon Docker path and then verify apply
behavior on native Fedora with a real desktop Polkit prompt.

For package apply behavior, use test packages that are safe to install and
remove in the test environment.
