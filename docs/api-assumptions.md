# External API Assumptions

This document records external API behavior that DNF UI relies on.

It is meant to keep maintenance grounded in real libdnf5, GTK, GIO, D-Bus, and
Polkit behavior instead of unverified assumptions. When a critical flow changes,
update the matching entry here and make sure the linked source still supports
the code.

## Source Priority

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

## Source Links

- libdnf5 C++ API overview: <https://dnf5.readthedocs.io/en/latest/api/c%2B%2B/libdnf5.html>
- GIO `GTask`: <https://docs.gtk.org/gio/class.Task.html>
- GIO `GCancellable`: <https://docs.gtk.org/gio/class.Cancellable.html>
- GIO `GDBusConnection::register_object`: <https://docs.gtk.org/gio/method.DBusConnection.register_object.html>
- GIO `GDBusConnection::signal_subscribe`: <https://docs.gtk.org/gio/method.DBusConnection.signal_subscribe.html>
- GIO `GDBusMethodInvocation`: <https://docs.gtk.org/gio/class.DBusMethodInvocation.html>
- Polkit `PolkitAuthority`: <https://polkit.pages.freedesktop.org/polkit/PolkitAuthority.html>

## libdnf5 Package Queries

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
- The upgradable package list is a read-only candidate view. The transaction
  preview remains the source of truth for what would actually be installed,
  upgraded, downgraded, reinstalled, or removed.

Tests:

- `Upgradeable package rows are classified as upgradeable`
- `Cancelled upgradeable package list returns no results`

Maintenance check:

- If the upgradable list changes, verify `package_query.hpp` in the build image
  and rerun the backend tests.

## libdnf5 Upgrade All

Code:

- [src/dnf_backend/dnf_transaction.cpp](../src/dnf_backend/dnf_transaction.cpp)
- [src/service/transaction_service_workers.cpp](../src/service/transaction_service_workers.cpp)

Assumption:

- `Goal::add_rpm_upgrade()` without a package spec adds an upgrade job for all
  installed packages, unless limited by the provided goal settings.

Current local source:

- `/usr/include/libdnf5/base/goal.hpp`

Why this matters:

- Upgrade All is intentionally sent as one all-installed-packages upgrade job.
  The GUI does not expand it into many package specs.
- Do not document this as bit-for-bit equivalence with every possible `dnf`
  command-line configuration, plugin, or option. The maintained guarantee is
  that DNF UI sends libdnf5's all-installed-packages RPM upgrade request through
  the app's configured backend and requires a preview before apply.

Tests:

- transaction request validation tests
- transaction preview tests
- transaction service preview tests
- empty upgrade-all preview test path

Maintenance check:

- If upgrade-all behavior changes, verify `goal.hpp`, then test preview and
  apply in Docker before any native system test.

## Shared libdnf5 Base Access

Code:

- [src/base_manager.cpp](../src/base_manager.cpp)
- [src/base_manager.hpp](../src/base_manager.hpp)

Assumptions:

- DNF UI owns synchronization around its shared `libdnf5::Base`.
- Read-only package queries take `BaseManager::acquire_read()`.
- `BaseManager::acquire_read()` is serialized with an exclusive guard. Do not
  change it back to shared locking unless libdnf5 `Base` and `PackageQuery`
  concurrent access has been verified against the local libdnf5 version.
- Transaction preview and apply take `BaseManager::acquire_write()` because
  transaction resolution and apply operate on shared libdnf5 state.
- Changelog lookups read installed packages from the shared Base first because
  rpmdb changelog metadata does not need repo `other` metadata.
- Changelog lookups use `BaseManager::acquire_changelog_read()` only when no
  installed package matches, so repo `other` metadata is loaded only for the
  short-lived available-package changelog query.
- The backend installed snapshot mutex must not be held at the same time as a
  `BaseManager` read or write guard.

Current local source:

- `/usr/include/libdnf5/base/base.hpp`

Why this matters:

- The app avoids relying on undocumented cross-thread behavior of one shared
  libdnf5 `Base`.
- The explicit lock ordering prevents deadlocks between package query code and
  installed-state cache code.

Tests:

- `BaseManager generation increments on rebuild`
- `acquire_read returns current generation snapshot`
- installed package cache consistency tests

Maintenance check:

- Any change that touches `BaseManager`, `dnf_state.cpp`, or transaction
  resolution should be reviewed for lock ordering.

## GTK and GIO Background Work

Code:

- [src/ui/package_query_controller.cpp](../src/ui/package_query_controller.cpp)
- [src/ui/package_info_controller.cpp](../src/ui/package_info_controller.cpp)
- [src/ui/widgets.cpp](../src/ui/widgets.cpp)
- [src/app.cpp](../src/app.cpp)

Assumptions:

- `GTask` completion callbacks run in the thread-default main context where the
  task was created.
- DNF UI creates UI tasks from the GTK thread, so finish callbacks may update GTK
  widgets after they validate that the result still applies.
- `g_task_run_in_thread()` runs synchronous backend work on a worker thread.
- `GCancellable` is cooperative. Worker code must check it at safe points.

Why this matters:

- UI code must not update GTK widgets directly from worker threads.
- Stop buttons cancel task state, but long libdnf5 calls may only stop after the
  next cancellable check.
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

## GDBus Transaction Objects

Code:

- [src/service/transaction_service.cpp](../src/service/transaction_service.cpp)
- [src/service/transaction_service_manager.cpp](../src/service/transaction_service_manager.cpp)
- [src/service/transaction_service_request_objects.cpp](../src/service/transaction_service_request_objects.cpp)
- [src/service/transaction_service_signals.cpp](../src/service/transaction_service_signals.cpp)
- [src/service/transaction_service_introspection.cpp](../src/service/transaction_service_introspection.cpp)
- [src/transaction_service_client.cpp](../src/transaction_service_client.cpp)

Assumptions:

- `g_dbus_connection_register_object()` dispatches vtable callbacks in the
  thread-default main context used when the object was registered.
- `GDBusMethodInvocation` is the object used to return a method result or error.
- `g_dbus_method_invocation_return_value()` finishes a method call and takes
  ownership of the invocation.
- Signal subscriptions invoke callbacks in the subscribing thread's
  thread-default main context.

Why this matters:

- The service may keep an apply invocation while Polkit authorization is pending,
  but each D-Bus method call must still receive exactly one final reply.
- The GUI client can wait for `Finished` signals while also handling service
  disappearance as a normal error path.

Tests:

- transaction service client tests
- transaction service preview smoke test
- transaction service cancel smoke test
- transaction service disconnect tests

Maintenance check:

- If a method stores `GDBusMethodInvocation`, verify every success, failure,
  shutdown, and release path returns or clears it exactly once.

## Polkit Authorization

Code:

- [src/service/transaction_service_authorization.cpp](../src/service/transaction_service_authorization.cpp)
- [packaging/com.fedora.dnfui.policy](../packaging/com.fedora.dnfui.policy)

Assumptions:

- Polkit authorization checks decide whether a subject is allowed to perform one
  action id.
- `POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION` is appropriate only
  for requests that come from a user action.
- DNF UI applies this only to the system-bus Apply step.
- The session-bus service path skips Polkit for development and tests.

Why this matters:

- The GUI stays unprivileged.
- Preview can be prepared without granting package modification rights.
- The privileged step is limited to the user-confirmed Apply operation.

Tests:

- system bus service smoke tests
- session bus service tests
- native manual Polkit prompt test

Maintenance check:

- Any new privileged operation needs a policy decision before implementation. It
  should not be added only because the session-bus development path works.
- Session-bus tests do not prove Polkit behavior. Policy or authorization
  changes need the matching system-bus tests and a native prompt check when
  possible.

## Dependency Update Checklist

When updating Fedora base images, libdnf5, GTK, GLib, GIO, or Polkit, revalidate
the affected entries in this document.

Minimum checks:

- confirm the local headers still contain the API behavior this document cites
- run `git diff --check`
- run the affected unit tests
- run the matching Docker service tests for transaction-service changes
- run a native Polkit prompt test for authorization changes when possible

## Documentation and Test Gaps

Some behavior is intentionally hard to prove in unit tests:

- real native Polkit prompts
- real host package transactions
- exact available update contents of a live Fedora repository
- long-running network or mirror behavior

When changing those areas, combine automated Docker tests with a short manual
verification note in the pull request or commit message.
