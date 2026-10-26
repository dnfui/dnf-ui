# Testing

This document explains the test layout and what each group protects.

For the external API behavior these tests help protect, see [External API assumptions](api-assumptions.md).

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

- [test/unit/test_base_manager.cpp](../test/unit/test_base_manager.cpp)
- [test/unit/test_installed_state.cpp](../test/unit/test_installed_state.cpp)
- [test/unit/test_package_details.cpp](../test/unit/test_package_details.cpp)
- [test/unit/test_package_queries.cpp](../test/unit/test_package_queries.cpp)
- [test/unit/test_config.cpp](../test/unit/test_config.cpp)
- [test/unit/test_daemon_upgrade_state.cpp](../test/unit/test_daemon_upgrade_state.cpp)
- [test/unit/test_dnf5daemon_client.cpp](../test/unit/test_dnf5daemon_client.cpp)
- [test/unit/test_pending_transaction_action_rows.cpp](../test/unit/test_pending_transaction_action_rows.cpp)
- [test/unit/test_package_query_cache.cpp](../test/unit/test_package_query_cache.cpp)
- [test/unit/test_package_table_export.cpp](../test/unit/test_package_table_export.cpp)
- [test/unit/test_package_table_sort.cpp](../test/unit/test_package_table_sort.cpp)
- [test/unit/test_package_table_status.cpp](../test/unit/test_package_table_status.cpp)
- [test/unit/test_package_table_upgrade_rows.cpp](../test/unit/test_package_table_upgrade_rows.cpp)
- [test/unit/test_pending_transaction_request.cpp](../test/unit/test_pending_transaction_request.cpp)
- [test/unit/test_repository_apply_feedback.cpp](../test/unit/test_repository_apply_feedback.cpp)
- [test/unit/test_repository_apply_model.cpp](../test/unit/test_repository_apply_model.cpp)
- [test/unit/test_repository_apply_operation.cpp](../test/unit/test_repository_apply_operation.cpp)
- [test/unit/test_repository_service_client.cpp](../test/unit/test_repository_service_client.cpp)
- [test/unit/test_search.cpp](../test/unit/test_search.cpp)
- [test/unit/test_transaction_history.cpp](../test/unit/test_transaction_history.cpp)
- [test/unit/test_transaction_request.cpp](../test/unit/test_transaction_request.cpp)
- [test/unit/test_offline.cpp](../test/unit/test_offline.cpp)
- [test/unit/test_offline_transaction.cpp](../test/unit/test_offline_transaction.cpp)

The backend tests are grouped by responsibility: BaseManager initialization, caching, and repository
fallback; installed snapshots and package state; search, browse, and version merging; and package
information, dependencies, and files. All four files run in the existing `dnfui-tests` executable.

These tests protect:

- package search and merge behavior
- Latest only and all-version package query behavior
- config file parsing and fallback behavior
- installed snapshot behavior, including passive refresh without dropping the warmed Base
- unneeded-package filtering, preservation of the installed snapshot, and cancellation
- pending transaction action row selection for install, upgrade, downgrade, remove, and reinstall
- package table CSV export formatting
- package table column text and sorting behavior
- daemon upgrade row construction with missing or mismatched metadata, preserved target identity and
  generation, and full NEVRA fallback, without GTK windows or backend queries
- dnf5daemon transaction preview parsing, upgrade-target parsing, and failure handling
- repository Apply planning, write outcome classification, and final-state verification
- repository Apply status messages and colors for successful, partial, failed, and unverified changes, and for
  different backend rebuild results, without GTK windows
- repository Apply workflow with substituted repository and backend operations, including initial read
  failures, disappeared repositories, already-matching state, partial writes, failed rebuilds, and failed
  final-state verification without changing system repositories
- dnf5daemon repository list parsing and repository enable or disable calls
- daemon upgrade snapshot ownership, stale-state invalidation, and current-target validation; these state tests
  also cover the snapshot rules used by the quiet upgrade-count task
- transaction history row formatting, action labels, and page cursor behavior
- history filter text trimming, optional local date bounds, inclusive end dates, invalid dates, and reversed ranges
- transaction request validation
- offline and cached metadata behavior
- offline preview/apply mode checks, prepared-state inspection, verified cleanup and unsupported daemon replies
  through a private D-Bus test fixture without changing system packages

Signing-key dialog tests cover key details without a GTK display and rejection when the task was already
cancelled. The existing dialog tests that open GTK windows require a display.

## Daemon smoke tests

The app now talks to DNF5 dnf5daemon for privileged transactions.
Use Docker and native Fedora testing to verify upgrade-target listing, preview, apply, cancel,
failure, session cleanup, and repository enable or disable behavior.

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

That Docker target also creates a disposable disabled repository and verifies that the repository client can
enable it and disable it again through fresh dnf5daemon sessions.
The test repository does not need package metadata because repository listing and enabled-state changes
do not load available package data.

Run the dnf5daemon transaction client tests on native Fedora in a disposable test environment.
This target skips daemon apply tests by default, but the test setup uses `sudo dnf5` to remove and
restore `cowsay` or the configured test package:

```sh
make dnf5daemontest
```

Set `DNFUI_TEST_DNF5DAEMON_INSTALL_SPEC` to use another package. On exit, the script attempts to restore
the test package's previous installed state.
The optional downgrade preview test runs when `DNFUI_TEST_DNF5DAEMON_DOWNGRADE_SPEC` names a safe installed package version
that can be downgraded in the test environment.
To also test real dnf5daemon apply calls, use a disposable Fedora VM or test machine. This command performs
package transactions in addition to the setup changes described above and requires dnf5daemon authorization:

```sh
make dnf5daemonapplytest
```

On a native Fedora test machine, installing `dnf5daemon-server-polkit` lets the apply tests run without a
desktop authorization prompt.

Without that package, run `make dnf5daemonapplytest` from the desktop session where a Polkit authentication
agent can show the authorization dialog. Running the apply target over SSH may fail with `Not authorized`.

### Native repository key prompt test

Use a disposable Fedora VM or test machine. Do not use your main system for this test.

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

The key prompt can appear while preparing the preview or while applying the transaction.
Both paths must ask before trusting the key.

Reject test:

- Click `Reject` in the DNF UI key prompt.
- The transaction should fail.
- The package should not be installed.
- `utils/setup_gpg_key_prompt_test_repo.sh status` should not list an imported test key.

Close test:

- Start the same transaction in DNF UI again.
- Close the DNF UI key prompt without trusting the key.
- The package should not be installed.
- `utils/setup_gpg_key_prompt_test_repo.sh status` should not list an imported test key.

The Apply progress window is not user-cancellable. Cancellation after UI approval but before daemon
confirmation is covered by a unit test.

Run `utils/setup_gpg_key_prompt_test_repo.sh restore` before the accept test.

Accept test:

- Run `utils/setup_gpg_key_prompt_test_repo.sh setup`.
- Start the same transaction in DNF UI again.
- Click `Trust Key` in the DNF UI key prompt.
- The transaction should continue.
- `utils/setup_gpg_key_prompt_test_repo.sh status` should show the package and imported test key.

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

The Makefile is a task runner. Meson owns build configuration and test definitions.

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

The default Memcheck setup fails on definite and indirect leaks from this project. Reachable and possible leak
noise from GLib, GTK, DNF, and related libraries is suppressed in `utils/valgrind-dnfui.supp`.

Useful options:

```sh
MEMCHECK_SMOKE_FILTER="Transaction request validation rejects an empty request" make memcheck
MEMCHECK_SMOKE_TIMEOUT=5m make memcheck
MEMCHECK_SMOKE_TIMEOUT=10m make memcheck
```

### Package table memory regression check

Use this before releases that change package table storage, package query workers, all-version listing, or
List Upgradable row handling:

1. Start DNF UI and record process memory after the first window is idle:

   ```sh
   pid="$(pgrep -n dnfui)"
   grep -E 'Rss|Pss|Private' "/proc/$pid/smaps_rollup"
   ```

2. Run `List Packages` ten times, waiting for each run to finish before recording memory again.
3. Repeat with `List Installed`, a broad `Search`, and `List Upgradable`.
4. Repeat Search and List Packages with Latest only disabled.
5. Alternate Latest only and all-version searches to verify all-version rows do not accumulate outside the visible table.
6. Alternate `List Upgradable` and `List Packages` a few times to verify old daemon-backed rows are released.
7. A high but stable value is acceptable. Continued substantial growth after every completed run is a release blocker.
8. Run `make memcheck-app` once if memory keeps growing or if table row ownership changed.

## Docker notes

- `make dockerrun` starts a system bus in the container and uses dnf5daemon
- `make dockerdnf5daemontest` runs upgrade-target listing, preview, downgrade preview when configured, apply,
  remove, reinstall, and failure checks against dnf5daemon
- The dnf5daemon failure checks include applying a prepared session after another session changed package
  state. The stale prepared session must fail and remain releasable.
- Use native Fedora to test the real desktop Polkit prompt

## Fedora review checks

Fedora package review expects `rpmlint` output for the source RPM and all binary RPMs produced by the build.
It is also normal to check that the source RPM builds in `mock`.

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

Use the Fedora release and architecture that match the package review you are preparing.
Running `mock` may require the user to be a member of the `mock` group.

Reference: <https://docs.fedoraproject.org/en-US/packaging-guidelines/ReviewGuidelines/>

## What to test after changes

For documentation-only changes, run `git diff --check`.

For comments inside C++ source files, also run a target that compiles the changed files.

For backend query changes, run the relevant Catch2 tests.

For transaction changes, test the dnf5daemon Docker path and then verify apply behavior on native Fedora with
a real desktop Polkit prompt.

For package apply behavior, use test packages that are safe to install and remove in the test environment.

## Quiet upgrade-count checks

After changes to `package_query_upgrade_indicator.cpp`, verify on a desktop that a successful startup check
updates the List Upgradable button count without loading the package table. Run Refresh Repositories and check
that the count updates again after the refresh completes. Check that starting a package list or transaction
preview does not let a quiet count refresh interrupt active work; pressing List Upgradable must still fetch
and display the full daemon-reported upgrade list.

## Unneeded package listing checks

1. Choose Package > List Unneeded and compare the results with `dnf5 repoquery --unneeded`.
2. Verify the list is read-only and only changes packages after explicit removal marking, preview, and approval.
3. While listing, use the List Installed button in Stop mode. Check that cancelling leaves the previous
   displayed results unchanged and the controls usable.
4. Change the installed package set, then reload List Unneeded and verify the current results are shown.
5. Check that package status and pending removal actions still resolve correctly after switching between
   List Unneeded and List Installed.
6. Check the listing in installed-only metadata fallback mode. Protected or install-only packages in the
   results must never be assumed safe to remove; the daemon still decides the resolved transaction.

## Multi-version package browsing checks

Use these checks after changes to Latest only, package queries, package table display, or pending action identity:

1. Start with Latest only enabled.
2. Run Search and List Packages and verify the compact one-row-per-package view.
3. Disable Latest only and run the same query again.
4. Verify that distinct exact NEVRAs can appear as separate rows.
5. Verify that duplicate repository copies of the same exact NEVRA are hidden.
6. Verify that older repository versions show `Older in repository`.
7. Verify that intermediate newer versions can be selected for Details but cannot be marked for upgrade.
8. Verify that the newest available version remains the only normal upgrade target.
9. Verify that downgradeable rows mark a pending downgrade using the selected exact NEVRA.
10. Verify that removing or reinstalling one exact installed NEVRA does not change pending actions for another
    installed NEVRA with the same name and architecture.
11. Verify that List Installed, List Unneeded, List Upgradable, Upgrade All, and Mark Listed Upgrades do not
    depend on the Latest only checkbox.
12. For an install-only package such as a kernel, verify that two distinct exact versions can be marked for
    installation together.
13. Verify that marking the same exact install-only NEVRA twice does not add a duplicate pending action.
14. Verify that switching Latest only on or off does not convert a pending exact install-only install into an
    upgrade or downgrade.
15. Verify that Unmark removes a pending exact install-only action instead of converting it to another action.
16. Verify that normal package upgrade behavior is unchanged.
17. Verify that the transaction preview contains both selected exact install-only NEVRAs when two versions are selected.
