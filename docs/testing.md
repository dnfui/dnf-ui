# Testing

This document explains the test layout and what each group protects.

For the external API behavior these tests help protect, see
[External API assumptions](api-assumptions.md).

## Test types

The project uses:

- Catch2 tests under [test/unit](../test/unit)
- shell smoke tests under [test/functional](../test/functional)
- Docker helpers under [docker](../docker)

The Catch2 tests are the fastest place to check backend and client behavior.
The shell smoke tests exercise the transaction service through D-Bus.

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
- [test/unit/test_transaction_service_preview_formatter.cpp](../test/unit/test_transaction_service_preview_formatter.cpp)
- [test/unit/test_transaction_service_request_parser.cpp](../test/unit/test_transaction_service_request_parser.cpp)
- [test/unit/test_transaction_service_validation.cpp](../test/unit/test_transaction_service_validation.cpp)
- [test/unit/test_transaction_service_client.cpp](../test/unit/test_transaction_service_client.cpp)
- [test/unit/test_offline.cpp](../test/unit/test_offline.cpp)

These tests protect:

- package search and merge behavior
- config file parsing and fallback behavior
- installed snapshot behavior
- package action row selection for install, upgrade, remove, and reinstall
- transaction preview behavior, including fail-closed preview building
- transaction request validation
- transaction service D-Bus request parsing
- transaction service request validation
- service client error handling
- offline and cached metadata behavior

## Service smoke tests

The service tests run the transaction service through D-Bus.

Automated service tests use a separate `dnfui-service-tests` binary. That
binary is compiled with the service test hooks enabled. The installed
`dnfui-service` binary is built without those hooks and is the only service
binary that gets installed.

Important scripts:

- [test/functional/test_transaction_service_preview.sh](../test/functional/test_transaction_service_preview.sh)
- [test/functional/test_transaction_service_cancel.sh](../test/functional/test_transaction_service_cancel.sh)
- [test/functional/test_transaction_service_apply.sh](../test/functional/test_transaction_service_apply.sh)
- [test/functional/test_transaction_service_preview_failure.sh](../test/functional/test_transaction_service_preview_failure.sh)
- [test/functional/test_transaction_service_system_bus.sh](../test/functional/test_transaction_service_system_bus.sh)
- [test/transaction_service_system_bus_client.cpp](../test/transaction_service_system_bus_client.cpp)

These tests protect:

- preview success
- preview cancellation
- preview failure handling
- apply flow
- system bus authorization path
- disconnect cleanup

The native installed-service smoke tests use a dedicated
`dnfui-service-smoke-client` helper. That helper keeps one real system bus
connection alive across `StartTransaction`, preview reads, `Apply`, and
`Release`, so the installed service keeps its normal request ownership checks.

The Docker system bus preview and apply tests still use `gdbus` as a
short-lived client for each method call. They set
`SERVICE_TEST_DISABLE_AUTO_RELEASE=1` on the test-only service binary so the
shell tests can inspect the request object after the start call exits and keep
request ownership checks compatible with that test shape. The installed service
binary is built without that test-only escape hatch.

Those Docker system bus tests also run without an active desktop session, so
the test-only service binary can be started with
`SERVICE_TEST_DISABLE_PREVIEW_AUTHORIZATION=1`. That switch is test-only and is
not present in the installed service binary.

## Common commands

Run the native test suite:

```sh
make test
```

Run the full native test matrix, including transaction service smoke tests:

```sh
SERVICE_TEST_INSTALL_SPEC=cowsay make nativetests
```

Run the normal Docker Catch2 test set:

```sh
make dockertest
```

Use Podman for the same container test target:

```sh
CONTAINER_RUNTIME=podman make dockertest
```

Run the main Docker-backed service and Catch2 test matrix:

```sh
make dockertests
```

Run the offline cached metadata smoke tests separately:

```sh
make dockerofflinetest
```

Run the session bus service preview smoke test:

```sh
make dockerservicetest
```

Run the other session bus service smoke tests:

```sh
make dockerservicepreviewfailuretest
make dockerservicecanceltest
make dockerserviceapplytest
```

Run the system bus service smoke tests:

```sh
make dockerservicesystemtest
make dockerservicesystemdisconnecttest
make dockerservicesystemapplytest
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

Run the automated test binary under Valgrind Memcheck:

```sh
make memcheck-tests
```

`make memory-check` currently runs the full automated Memcheck target.

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
MEMCHECK_TEST_FILTER="Search returns empty for impossible package name" make memcheck-tests
MEMCHECK_TEST_TIMEOUT=10m make memcheck-tests
MEMCHECK_TRACK_FDS=yes make memcheck-tests
MEMCHECK_GEN_SUPPRESSIONS=yes make memcheck-tests
```

## Docker notes

- `make dockerrun` uses the session bus service path for convenience
- Use the `dockerservicesystem*` targets to test the real system bus authorization flow
- Use native Fedora to test the real desktop Polkit prompt

## Fedora review checks

Fedora package review expects `rpmlint` output for the source RPM and all binary
RPMs produced by the build. It is also normal to check that the source RPM builds
in `mock`.

Build RPMs and run `rpmlint`:

```sh
make rpm
rpmlint dnf-ui-latest.src.rpm rpmbuild/RPMS/*/*.rpm
```

Keep the `rpmlint` output so it can be included in the Fedora package review.

Build the source RPM in a clean Fedora build environment:

```sh
make srpm
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

For backend query changes, run the Catch2 tests through `make dockertest`.

For transaction service changes, run `make dockertests` when possible. For a
focused check, choose the specific service target that matches the changed flow.
If the change touches system bus, Polkit, or client disconnect behavior, include
the matching system bus target.

For package apply behavior, use test packages that are safe to install and
remove in the test environment.
