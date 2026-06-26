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
- `PackageQuery::filter_upgrades()` keeps available packages that are upgrades to installed packages.
- `PackageQuery::filter_latest_evr()` limits visible candidates to the latest EVR.

Current local source:

- `/usr/include/libdnf5/rpm/package_query.hpp`

Why this matters:

- `dnf_backend_get_upgradeable_package_rows_interruptible` depends on
  `filter_upgrades()` so the app follows libdnf5's upgrade-candidate semantics
  instead of maintaining its own version comparison rules.
- The local upgradable package query is a read-only candidate query. The UI checks the List Upgradable view against the resolved dnf5daemon Upgrade All preview by package name and architecture before showing it. If libdnf5 reports no upgrade rows but dnf5daemon resolves upgrades, the app must show a clear error instead of a false empty list.
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

- [src/transaction_service_client_dbus.cpp](../src/transaction_service_client_dbus.cpp)
- [src/ui/pending_transaction_request.cpp](../src/ui/pending_transaction_request.cpp)

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

## Shared libdnf5 Base access

Code:

- [src/base_manager.cpp](../src/base_manager.cpp)
- [src/base_manager.hpp](../src/base_manager.hpp)

Assumptions:

- DNF UI owns synchronization around its shared `libdnf5::Base`.
- Read-only package queries take `BaseManager::acquire_read()`.
- `BaseManager::acquire_read()` is serialized with an exclusive guard. Do not
  change it back to shared locking unless libdnf5 `Base` and `PackageQuery`
  concurrent access has been verified against the local libdnf5 version.
- The remaining local backend transaction helpers take `BaseManager::acquire_write()` because transaction
  resolution and apply operate on shared libdnf5 state. The normal GUI transaction path uses dnf5daemon.
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

- [src/ui/package_query_controller.cpp](../src/ui/package_query_controller.cpp)
- [src/ui/package_info_controller.cpp](../src/ui/package_info_controller.cpp)
- [src/ui/widgets.cpp](../src/ui/widgets.cpp)
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

- [src/transaction_service_client.cpp](../src/transaction_service_client.cpp)
- [src/transaction_service_client_dbus.cpp](../src/transaction_service_client_dbus.cpp)
- [src/transaction_service_client_wait.cpp](../src/transaction_service_client_wait.cpp)

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
- Resolve failures are read through `org.rpm.dnf.v0.Goal.get_transaction_problems_string() -> (as)`.
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

- [src/transaction_service_client_dbus.cpp](../src/transaction_service_client_dbus.cpp)

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
