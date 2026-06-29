# Testing

This document explains the test layout and what each group protects.

For the external API behavior these tests help protect, see
[External API assumptions](api-assumptions.md).

## Test types

The project uses:

- Catch2 tests under [test/unit](../test/unit)
- Docker helpers under [docker](../docker)

The Catch2 tests are the fastest place to check backend and client behavior.

## Test dependencies

The test build needs:

- `pkgconfig(gio-2.0)`
- `pkgconfig(catch2-with-main)`

On Fedora, `catch-devel` provides `pkgconfig(catch2-with-main)`.

## Catch2 tests

Key files:

- [test/unit/test_backend.cpp](../test/unit/test_backend.cpp)
- [test/unit/test_config.cpp](../test/unit/test_config.cpp)
- [test/unit/test_dnf5daemon_client.cpp](../test/unit/test_dnf5daemon_client.cpp)
- [test/unit/test_pending_transaction_action_rows.cpp](../test/unit/test_pending_transaction_action_rows.cpp)
- [test/unit/test_package_query_cache.cpp](../test/unit/test_package_query_cache.cpp)
- [test/unit/test_package_table_sort.cpp](../test/unit/test_package_table_sort.cpp)
- [test/unit/test_pending_transaction_request.cpp](../test/unit/test_pending_transaction_request.cpp)
- [test/unit/test_search.cpp](../test/unit/test_search.cpp)
- [test/unit/test_transaction_request.cpp](../test/unit/test_transaction_request.cpp)
- [test/unit/test_offline.cpp](../test/unit/test_offline.cpp)

These tests protect:

- package search and merge behavior
- config file parsing and fallback behavior
- installed snapshot behavior
- pending transaction action row selection for install, upgrade, remove, and reinstall
- package table column text and sorting behavior
- dnf5daemon transaction preview parsing and failure handling
- transaction request validation
- offline and cached metadata behavior

## Daemon smoke tests

The app now talks to DNF5 dnf5daemon for privileged transactions.
Use Docker and native Fedora testing to verify preview, apply, cancel, failure,
and session cleanup behavior.

## Common commands

Run the native test suite:

```sh
make test
```

Run the offline cached metadata smoke tests separately:

```sh
make dockerofflinetest
```

Run the dnf5daemon transaction client tests in Docker:

```sh
make dockerdnf5daemontest
```

Run the dnf5daemon transaction client tests on native Fedora:

```sh
make dnf5daemontest
```

The native dnf5daemon tests may install and remove `cowsay` unless
`DNFUI_TEST_DNF5DAEMON_INSTALL_SPEC` is set to another package.
The script restores whether the test package was installed before the run.
Native apply tests are skipped by default because they require dnf5daemon
authorization. Run them explicitly with:

```sh
make dnf5daemonapplytest
```

On a native Fedora test machine, installing `dnf5daemon-server-polkit` lets the
apply tests run without a desktop authorization prompt.

Without that package, run `make dnf5daemonapplytest` from the desktop session
where a Polkit authentication agent can show the authorization dialog. Running
the apply target over SSH may fail with `Not authorized`.

### Native repository key prompt test

Use a disposable Fedora VM or test machine. Do not use your main system for this
test.

Create the local signed test repo:

```sh
utils/setup_gpg_key_prompt_test_repo.sh setup
```

Run DNF UI from the branch:

```sh
make run
```

Search for:

```sh
dnfui-gpg-test-package
```

The key prompt can appear while preparing the preview or while applying the
transaction. Both paths must ask before trusting the key.

Reject test:

- Click `Reject` in the DNF UI key prompt.
- The transaction should fail.
- The package should not be installed.
- `utils/setup_gpg_key_prompt_test_repo.sh status` should not list an imported
  test key.

Run `utils/setup_gpg_key_prompt_test_repo.sh restore` before the accept test.

Accept test:

- Run `utils/setup_gpg_key_prompt_test_repo.sh setup`.
- Start the same transaction in DNF UI again.
- Click `Trust Key` in the DNF UI key prompt.
- The transaction should continue.
- `utils/setup_gpg_key_prompt_test_repo.sh status` should show the package and
  imported test key.

Restore the disposable test machine:

```sh
utils/setup_gpg_key_prompt_test_repo.sh restore
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
- `make dockerdnf5daemontest` runs preview, apply, remove, reinstall, and failure checks against dnf5daemon
- Use native Fedora to test the real desktop Polkit prompt

## Fedora review checks

Fedora package review expects `rpmlint` output for the source RPM and all binary
RPMs produced by the build. It is also normal to check that the source RPM builds
in `mock`.

Build RPMs with the Make targets and run `rpmlint`:

```sh
make srpm
make rpm
rpmlint dnf-ui-latest.src.rpm rpmbuild/RPMS/*/*.rpm
```

Keep the `rpmlint` output so it can be included in the Fedora package review.

Build the source RPM in a clean Fedora build environment:

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

For backend query changes, run the relevant Catch2 tests.

For transaction changes, test the dnf5daemon Docker path and then verify apply
behavior on native Fedora with a real desktop Polkit prompt.

For package apply behavior, use test packages that are safe to install and
remove in the test environment.
