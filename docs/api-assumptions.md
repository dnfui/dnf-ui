# External API assumptions

This document records external API behavior that DNF UI relies on.

It is meant to keep maintenance grounded in real libdnf5, GTK, GIO, D-Bus, and
Polkit behavior instead of unverified assumptions. When a critical flow changes,
update the matching entry here and make sure the linked source still supports
the code.

## Source priority

Use sources in this order:

1. The installed headers and libraries used by the current build environment.
2. Official upstream API documentation.
3. Tests in this repository.
4. Manual verification notes for behavior that is hard to automate.

The Docker development image is useful because it shows the headers and library
versions the project is actually compiling against.

Useful checks in the target build environment:

```sh
./utils/meson_build.sh all
./utils/meson_build.sh tests
```

Inside the Docker image, critical libdnf5 declarations can be checked under
`/usr/include/libdnf5`.

## Source links

- libdnf5 C++ API overview: <https://dnf5.readthedocs.io/en/latest/api/c%2B%2B/libdnf5.html>
- dnf5daemon D-Bus API: <https://dnf5.readthedocs.io/en/latest/dnf_daemon/dnf5daemon_dbus_api.8.html>
- GIO `GTask`: <https://docs.gtk.org/gio/class.Task.html>
- GIO `GCancellable`: <https://docs.gtk.org/gio/class.Cancellable.html>
- GIO `GDBusConnection::register_object`: <https://docs.gtk.org/gio/method.DBusConnection.register_object.html>
- GIO `GDBusConnection::signal_subscribe`: <https://docs.gtk.org/gio/method.DBusConnection.signal_subscribe.html>
- GIO `GDBusMethodInvocation`: <https://docs.gtk.org/gio/class.DBusMethodInvocation.html>
- Polkit `PolkitAuthority`: <https://polkit.pages.freedesktop.org/polkit/PolkitAuthority.html>

## libdnf5 package queries

Code:

- [src/dnf_backend/dnf_query.cpp](../src/dnf_backend/dnf_query.cpp)

Assumptions:

- `PackageQuery::filter_available()` removes installed packages from the query.
- `PackageQuery::filter_latest_evr()` limits visible candidates to the latest EVR.

Current local source:

- `/usr/include/libdnf5/rpm/package_query.hpp`

Why this matters:

- List Upgradable uses dnf5daemon's upgrade target list first. libdnf5 is then used only to enrich those exact daemon-selected NEVRAs with package metadata.
- Missing libdnf5 metadata must not hide a daemon-reported upgrade. The UI should keep a basic daemon row rather than showing a false empty list.
- Upgradable rows are available package candidates. UI actions that remove or reinstall such a row
  must resolve the matching installed row by package name and architecture before building the pending action.
- Installed rows can also be classified as upgradable after repo annotation.
  Those rows carry the matching available upgrade NEVRA so UI action handling
  does not need a fresh libdnf query on the GTK thread.

Tests:

- `Upgradeable package rows are classified as upgradeable`
- `Cancelled upgradeable package list returns no results`

Maintenance check:

- If the upgradable list changes, verify `package_query.hpp` in the build image
  and rerun the backend tests.

## dnf5daemon upgrade requests

Code:

- [src/dnf5daemon_client/transaction_service_client_dbus.cpp](../src/dnf5daemon_client/transaction_service_client_dbus.cpp)
- [src/ui/transaction/pending_transaction_request.cpp](../src/ui/transaction/pending_transaction_request.cpp)

Assumption:

- dnf5daemon `upgrade` with explicit package specs marks those packages for
  upgrade in the daemon session.
- dnf5daemon `upgrade` with an empty package list prepares its native Upgrade
  All transaction.

Source:

- DNF5 dnf5daemon D-Bus API

Why this matters:

- Upgrade All calls dnf5daemon's native upgrade-all path by sending an empty
  package list to the daemon's `upgrade` method.
- Selected package upgrades must be sent to the daemon as upgrade specs, not
  install specs. DNF UI sends the package name and architecture for selected
  upgrades because exact candidate NEVRAs can fail with no match even though the
  package is correctly shown as upgradable.
- If the resolved preview would remove or replace DNF UI itself, the preview is
  rejected before the user can apply it. Normal upgrades are allowed.
- Do not document this as bit-for-bit equivalence with every possible `dnf`
  command-line configuration, plugin, or option. The maintained guarantee is
  that DNF UI sends the upgrade request through the app's configured backend and
  requires a preview before apply.

Tests:

- transaction request validation tests
- pending transaction request builder tests
- transaction preview tests
- empty upgrade-all preview test path

Maintenance check:

- If upgrade-all behavior changes, verify `goal.hpp`, then test preview and apply in Docker before any native system test.

## dnf5daemon upgrade target listing

Code:

- [src/dnf5daemon_client/transaction_service_client_dbus.cpp](../src/dnf5daemon_client/transaction_service_client_dbus.cpp)
- [src/upgrade/daemon_upgrade_state.cpp](../src/upgrade/daemon_upgrade_state.cpp)

Assumption:

- dnf5daemon `org.rpm.dnf.v0.rpm.Rpm.list` supports `scope="upgrades"` and `latest-limit=1`.
- The requested package attributes `name`, `epoch`, `version`, `release`, `arch`, `repo_id`, `nevra`, and `full_nevra` are supported package-list attributes.

Source:

- DNF5 dnf5daemon D-Bus API
- DNF5 dnf5daemon example code uses `Rpm.list` with `scope="upgrades"` and `latest-limit=1` to print available upgrades.

Why this matters:

- The daemon-owned upgrade refactor needs a read-only daemon snapshot of upgrade targets. This snapshot should come from dnf5daemon's package-list API, not from a resolved Upgrade All transaction preview.
- DNF UI keeps daemon upgrade specs and internal package identity separate. `name.arch` is the daemon upgrade spec. The internal package identity uses package name and architecture as separate values.
- Normal `nevra` is the application-facing package ID because it matches libdnf5 `PackageRow::nevra`. `full_nevra` is kept separately for callers that need the daemon's full epoch form.
- The shared daemon upgrade state stores complete package-list results by package name and architecture. It does not fetch daemon data itself.
- Successful List Upgradable results publish that shared daemon state from the GTK completion path after the result is accepted for display. The worker must not publish a successful snapshot before completion can still reject or cancel the result.
- List Upgradable performs fresh installed-package scans before and after the daemon target list is loaded. If the second scan detects a change, the daemon result must be rejected and the user must reload List Upgradable.
- A successful empty result means no daemon-reported upgrade targets. A failed request means upgrade information is unavailable. Those states must not be treated as the same thing.
- The shared daemon upgrade state exposes status separately from target rows. Only `READY` means the target map can be used as current upgrade information.
- Daemon upgrade results may only be published by the refresh that owns the active refresh ID. If package or repository state becomes stale before a daemon call returns, that old result must not become the current snapshot.
- Repository refresh start, successful transaction apply, and an installed-package refresh that observes a changed rpmdb mark daemon upgrade information stale before the UI allows more upgrade actions.
- Cancelled refresh workers must call the refresh-ID scoped abandon operation. They must not call `mark_stale()`, because that could invalidate newer refresh work.
- If dnf5daemon returns exact duplicate upgrade targets for the same package identity, DNF UI collapses them. If it returns conflicting targets for the same package identity, DNF UI rejects the whole snapshot instead of choosing one.
- The final transaction preview still comes from dnf5daemon resolve before apply. The package-list API is the upgrade-state snapshot, not permission to skip preview.

Tests:

- `dnf5daemon client lists upgrade targets`
- daemon upgrade state unit tests

Maintenance check:

- If dnf5daemon package-list fields or scope names change, update the target parser and rerun the dnf5daemon client tests in Docker.

## Shared libdnf5 Base access

Code:

- [src/dnf_backend/base_manager.cpp](../src/dnf_backend/base_manager.cpp)
- [src/dnf_backend/base_manager.hpp](../src/dnf_backend/base_manager.hpp)

Assumptions:

- DNF UI owns synchronization around its shared `libdnf5::Base`.
- Read-only package queries take `BaseManager::acquire_read()`.
- `BaseManager::acquire_read()` is serialized with an exclusive guard. Do not
  change it back to shared locking unless libdnf5 `Base` and `PackageQuery`
  concurrent access has been verified against the local libdnf5 version.
- Transaction preview and apply use dnf5daemon. The GTK process does not keep a
  local libdnf transaction apply path.
- Changelog lookups read installed packages from the shared Base first because rpmdb changelog metadata
  does not need repo `other` metadata.
- Available package changelogs use a temporary Base with repo `other` metadata.
  The shared Base read lock must be released before that temporary Base is loaded.
- The backend installed snapshot mutex must not be held at the same time as a `BaseManager` read or write guard.

Current local source:

- `/usr/include/libdnf5/base/base.hpp`

Why this matters:

- The app avoids relying on undocumented cross-thread behavior of one shared
  libdnf5 `Base`.
- The explicit lock ordering prevents deadlocks between package query code and installed-state cache code.

Tests:

- `BaseManager generation increments on rebuild`
- `acquire_read returns current generation snapshot`
- installed package cache consistency tests

Maintenance check:

- Any change that touches `BaseManager`, `dnf_state.cpp`, or transaction
  resolution should be reviewed for lock ordering.

## GTK and GIO background work

Code:

- [src/ui/package_query/package_query_controller.cpp](../src/ui/package_query/package_query_controller.cpp)
- [src/ui/details/package_details_controller.cpp](../src/ui/details/package_details_controller.cpp)
- [src/ui/common/widgets.cpp](../src/ui/common/widgets.cpp)
- [src/app.cpp](../src/app.cpp)

Assumptions:

- `GTask` completion callbacks run in the thread-default main context where the task was created.
- DNF UI creates UI tasks from the GTK thread, so finish callbacks may update GTK
  widgets after they validate that the result still applies.
- `g_task_run_in_thread()` runs synchronous backend work on a worker thread.
- `GCancellable` is cooperative. Worker code must check it at safe points.
- libdnf repository download callbacks can stop the current transfer by returning `ABORT`.

Why this matters:

- UI code must not update GTK widgets directly from worker threads.
- Stop buttons cancel task state, but long libdnf5 calls may only stop after the next cancellable check.
- Repository refresh Stop can interrupt repository downloads only when libdnf
  reaches a download callback.
- Search and package list Stop can interrupt waiting for `BaseManager::acquire_read`,
  but it still cannot kill arbitrary libdnf work that is already running.
- DNF UI must not assume cancellation kills a worker thread. Cancelled workers
  can still finish later, so completion callbacks must be safe to ignore.
- Completion callbacks must check destroyed widgets, selected NEVRA, backend
  generation, or request id before applying results.

Tests:

- cancelled package query tests
- package details generation checks
- service client disconnect and cancellation tests

Maintenance check:

- Any new slow UI work should follow the existing `GTask` pattern and should
  have either cancellation coverage or a clear reason why cancellation is not
  supported.

## dnf5daemon D-Bus transaction flow

Code:

- [src/dnf5daemon_client/transaction_service_client.cpp](../src/dnf5daemon_client/transaction_service_client.cpp)
- [src/dnf5daemon_client/transaction_service_client_dbus.cpp](../src/dnf5daemon_client/transaction_service_client_dbus.cpp)
- [src/dnf5daemon_client/transaction_service_client_wait.cpp](../src/dnf5daemon_client/transaction_service_client_wait.cpp)

Checked against:

- Fedora Docker image package `dnf5daemon-server-5.4.2.1-1.fc44.x86_64`
- official dnf5daemon D-Bus API documentation linked above
- local D-Bus introspection using the same connection that opened the session

Assumptions:

- dnf5daemon is available on the system bus as `org.rpm.dnf.v0`.
- The session manager object path is `/org/rpm/dnf/v0`.
- Sessions are opened through `org.rpm.dnf.v0.SessionManager.open_session(a{sv}) -> (o)`.
- Manual repository refresh opens a session with `load_available_repos=false`
  and `load_system_repo=false`, then explicitly expires and reloads repositories.
- Session cleanup uses `org.rpm.dnf.v0.SessionManager.close_session(o) -> (b)`.
- Package specs are marked on the session through these rpm interface methods:
  `install(as, a{sv})`, `remove(as, a{sv})`, `reinstall(as, a{sv})`, and `upgrade(as, a{sv})`.
- Preview is resolved through `org.rpm.dnf.v0.Goal.resolve(a{sv}) -> (a(sssa{sv}a{sv})u)`.
- Resolver result `0` means success, `1` means success with warnings, and `2`
  means resolve failure.
- Resolve warnings and failures are read through `org.rpm.dnf.v0.Goal.get_transaction_problems_string() -> (as)`.
- Resolve uses `interactive=true` because dnf5daemon may need to request
  repository signing key approval while loading repositories.
- Apply is started through `org.rpm.dnf.v0.Goal.do_transaction(a{sv}) -> ()`.
- Apply is started with `interactive=true` because the daemon may need authorization while running the transaction.
- During resolve or apply, dnf5daemon may emit `repo_key_import_request`. The
  client must answer with
  `org.rpm.dnf.v0.rpm.Repo.confirm_key_with_options(s, b, a{sv})`.
- DNF UI must not approve a key without user consent.
- Signal subscriptions invoke callbacks in the subscribing thread's
  thread-default main context.
- A dnf5daemon session is tied to the D-Bus connection that created it.
- DNF UI rejects previews that would remove or replace the running app package.
  Normal package upgrades are allowed.
- DNF UI rejects previews that would remove or replace `dnf5daemon-server`,
  because later package changes depend on that daemon.
- The app keeps one shared system bus connection so the session used for preview
  is still valid when the user later applies.
- A resolved preview is not a system-wide DNF lock. If another package tool
  changes state before Apply, the apply call must fail and require a new preview.
- `do_transaction` may need longer than a normal D-Bus timeout because the user
  can leave the Polkit prompt open.
- Unsupported daemon transaction item types or actions must fail preview instead
  of being hidden.
- The progress window listens to selected dnf5daemon signals:
  `download_add_new`, `download_progress`, `download_end`,
  `download_mirror_failure`, `transaction_before_begin`,
  `transaction_verify_start`, `transaction_transaction_start`,
  `transaction_action_start`, and `transaction_unpack_error`.

Why this matters:

- A closed or replaced D-Bus connection invalidates sessions created through it.
- The GUI must close daemon sessions after preview cancel, preview failure,
  apply success, and apply failure.
- The preview dialog must reflect the complete transaction action set that the
  app understands.
- The UI must not report timeout failure while dnf5daemon is still waiting for
  the user's Polkit answer.
- Repository signing key import is a user trust decision. The app must not
  accept a daemon key request without asking the user.
- The signal list is intentionally small. It gives the user useful transaction
  stages without turning the progress window into a debug log.

Tests:

- dnf5daemon transaction tests need to cover preview, apply, release, daemon
  failure, authorization failure, and disconnect behavior
- Repository signing key import needs a native manual test with a package
  signing key that has not already been imported.

Maintenance check:

- If any hard-coded daemon method, interface, object path, or reply signature is
  changed, verify it against the DNF5 dnf5daemon version DNF UI targets.
- If session handling changes, test preview cancel, apply success, apply failure,
  and window close while a session exists.

## Polkit authorization through dnf5daemon

Code:

- [src/dnf5daemon_client/transaction_service_client_dbus.cpp](../src/dnf5daemon_client/transaction_service_client_dbus.cpp)

Assumptions:

- dnf5daemon owns the Polkit check for privileged package changes.
- DNF UI asks dnf5daemon to apply with interactive authorization enabled.
- DNF UI does not install its own transaction Polkit policy.

Why this matters:

- The GUI stays unprivileged.
- Preview can be prepared without granting package modification rights.
- The privileged step is handled by Fedora's existing DNF daemon.

Tests:

- dnf5daemon apply tests
- native manual Polkit prompt test

Maintenance check:

- Changes to privileged apply behavior must be tested through dnf5daemon on
  native Fedora. Docker is useful, but it does not prove the desktop Polkit
  prompt path.

## Dependency update checklist

When updating Fedora base images, libdnf5, GTK, GLib, GIO, or dnf5daemon,
revalidate the affected entries in this document.

Minimum checks:

- confirm the local headers still contain the API behavior this document cites
- run `git diff --check`
- run the affected unit tests
- run the matching dnf5daemon transaction tests for transaction changes
- run a native Polkit prompt test for authorization changes when possible

## Documentation and test gaps

Some behavior is intentionally hard to prove in unit tests:

- real native Polkit prompts
- real host package transactions
- exact available update contents of a live Fedora repository
- long-running network or mirror behavior

When changing those areas, combine automated Docker tests with a short manual
verification note in the pull request or commit message.
