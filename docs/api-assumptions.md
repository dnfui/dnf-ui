# External API assumptions

This document records the external API behavior that **DNF UI** depends on, particularly where incorrect
assumptions could cause regressions.

It focuses on libdnf5, dnf5daemon, GTK, GIO, and Polkit. Internal implementation details are documented
in [Architecture](architecture.md), [Backend](backend.md), and [Transaction flow](transactions.md).

When changing code that depends on an external API, verify the relevant behavior against the installed
version and update this document if the assumptions change.

## API references and verification

The installed headers and libraries are the primary reference for the current build environment.
Official upstream documentation provides the API contracts, while repository tests and manual verification
establish the behavior **DNF UI** expects.

Useful upstream references:

- [libdnf5 C++ API](https://dnf5.readthedocs.io/en/latest/api/c%2B%2B/libdnf5.html)
- [dnf5daemon D-Bus API](https://dnf5.readthedocs.io/en/latest/dnf_daemon/dnf5daemon_dbus_api.8.html)
- [GIO API](https://docs.gtk.org/gio/)
- [Polkit documentation](https://polkit.pages.freedesktop.org/polkit/)

The Fedora development Docker image provides a consistent environment for checking installed headers,
D-Bus interfaces, and library behavior. It uses `fedora:latest`, so the installed API versions may change
when the image is rebuilt.

Build and run the native tests with:

```sh
make test
```

For daemon integration tests, use:

```sh
make dockerdnf5daemontest
```

See [Testing](testing.md) for the remaining test commands and requirements.

## libdnf5 package queries

Implementation: [dnf_query.cpp](../src/dnf_backend/dnf_query.cpp)

**DNF UI** uses `libdnf5::rpm::PackageQuery` to read package information without modifying system packages.

The following query behavior is important:

- `filter_available()` excludes installed package records.
- `filter_latest_evr()` keeps the latest EVR candidates.
- `filter_unneeded()` identifies installed packages no longer required by user-installed packages.

List Unneeded uses `filter_unneeded()` as a read-only dependency query. A listed package is not necessarily removable.
Actual removal must still pass through dnf5daemon transaction resolution, including protected-package checks.

List Upgradable works differently. dnf5daemon determines which upgrades are available, while libdnf5
supplies additional metadata for the reported package versions. Missing local metadata must not cause
a valid daemon-reported upgrade to disappear.

Package actions must also distinguish available upgrade candidates from installed packages.
Removing or reinstalling an installed version requires resolving the matching installed package by name and architecture.
Installed rows marked as upgradable retain their available upgrade NEVRA so the GTK thread does not need
another libdnf5 query.

The relevant API declarations are in `/usr/include/libdnf5/rpm/package_query.hpp`.

These assumptions are covered by the package query, installed-state, and unneeded-package tests.

## Local libdnf5 plugin policy

Implementation: [base_manager.cpp](../src/dnf_backend/base_manager.cpp)

Local libdnf5 `Base` objects are used for package browsing, metadata, installed-state inspection,
changelogs, and transaction history. They do not execute package transactions.

Every local `Base` disables libdnf5 plugins after loading configuration and before calling `Base::setup()`.
This includes temporary Bases and fallback modes. dnf5daemon remains responsible for transaction execution
and loads its own configured plugins.

This separation prevents a library compatibility problem when libdnf5 is upgraded while **DNF UI** is running.

The process continues using its already-loaded libdnf5 library, but a newly created `Base` could otherwise
load an updated plugin from disk. For example, the DNF 5.4.4.0 systemd-inhibit plugin requires `IPlugin2_2`,
which is not available in libdnf5 5.4.3.0.

Disabling local plugins avoids loading incompatible plugin code without affecting daemon transaction hooks
or changing system configuration.

There is one limitation: custom plugins that provide repository configuration only in memory are not
executed by local Bases. Their results may therefore differ from daemon behavior.
Supporting such plugins would require a solution that preserves library compatibility.
Do not re-enable plugins as a fallback for failed queries.

Relevant upstream references:

- [DNF5 plugin configuration](https://dnf5.readthedocs.io/en/latest/dnf5.conf.5.html#plugins)
- [libdnf5 plugin API, version 5.4.4.0][plugin-api]

The test `BaseManager reads package data without loading libdnf5 plugins` verifies this policy using
an unloadable plugin.

Plugin disabling must remain in the shared local Base factory and must not be applied to daemon
session configuration.

## Shared libdnf5 Base access

Implementation: [base_manager.cpp](../src/dnf_backend/base_manager.cpp) and
[base_manager.hpp](../src/dnf_backend/base_manager.hpp)

**DNF UI** owns synchronization around its shared `libdnf5::Base`. Package queries use
`BaseManager::acquire_read()`, which currently takes an exclusive guard even for read-only operations.

This is intentional. Concurrent access to a shared `Base` and its `PackageQuery` objects must not be
assumed safe without verifying that behavior against the installed libdnf5 version.

Two additional rules prevent deadlocks:

- The installed-package snapshot mutex must never be held together with a `BaseManager` read or write guard.
- The shared Base lock must be released before loading a temporary Base for available-package changelogs.

Installed-package changelogs can normally use the shared Base because rpmdb changelog data does not require
repository `other` metadata. Available-package changelogs use a temporary Base that loads this metadata.

The relevant upstream declarations are in `/usr/include/libdnf5/base/base.hpp`.

Changes to Base synchronization or installed-state caching must preserve these rules.
The BaseManager generation and installed snapshot tests cover the current behavior.

## dnf5daemon sessions and package requests

Implementation: [transaction_service_client_dbus.cpp][transaction-client]

**DNF UI** communicates with dnf5daemon on the system bus through `org.rpm.dnf.v0`.

The session manager uses `/org/rpm/dnf/v0` and provides:

```text
SessionManager.open_session(a{sv}) -> (o)
SessionManager.close_session(o) -> (b)
```

Daemon sessions are tied to the D-Bus connection that created them. **DNF UI** keeps a shared system
bus connection so a prepared transaction remains available between preview and Apply.

Sessions must be released when no longer needed, including after cancelled previews, failures,
completed transactions, and replaced requests. Releasing sessions is performed away from the GTK thread.

Selected package transactions open sessions with `load_system_repo=true`. Remove-only requests use
`load_available_repos=false`, while other package actions load available repositories.

### Package actions

dnf5daemon exposes the following RPM methods:

```text
install(as, a{sv})
upgrade(as, a{sv})
downgrade(as, a{sv})
remove(as, a{sv})
reinstall(as, a{sv})
```

Upgrade All calls `upgrade` with an empty package list, invoking the daemon's native upgrade-all behavior.

Selected upgrades use explicit `name.arch` package specifications rather than exact candidate NEVRAs.
Exact candidate requests can fail to match even when the package is correctly reported as upgradable.

Downgrades are different: **DNF UI** sends the exact selected older NEVRA because the user has chosen
a specific package version.

These requests are not guaranteed to behave identically to every possible DNF5 CLI configuration.
The daemon resolves the requested actions using its configuration, and the user must review the
resolved transaction before Apply.

The behavior is documented in the [DNF5 D-Bus API][daemon-api] and the
[upstream upgrade implementation][daemon-upgrade].

Request validation, upgrade-all previews, and selected upgrade and downgrade previews are covered
by unit and Docker integration tests.

## Upgrade target listing

Implementation:

- [transaction_service_client_dbus.cpp][transaction-client]
- [transaction_service_client_parser.cpp][transaction-parser]
- [src/upgrade/daemon_upgrade_state.cpp](../src/upgrade/daemon_upgrade_state.cpp)

List Upgradable and the startup upgrade indicator use the read-only `org.rpm.dnf.v0.rpm.Rpm.list` API.

The query requests:

```text
scope = "upgrades"
latest-limit = 1
```

It reads `name`, `epoch`, `version`, `release`, `arch`, `repo_id`, `nevra`, and `full_nevra`.

These fields and options are documented in the [DNF5 D-Bus API][daemon-api] and demonstrated in the
[upstream examples](https://dnf5.readthedocs.io/en/latest/dnf_daemon/examples.html).

The resulting package list is an upgrade snapshot, not a resolved transaction. **DNF UI** must not
create an Upgrade All preview merely to discover or count available upgrades.

The client keeps package name and architecture as separate identity fields. Normal `nevra` is used for
application package identity, while `full_nevra` is retained when the daemon's full epoch form is needed.

### Keeping upgrade information current

Upgrade information is stored in a shared snapshot. Only a snapshot with status `READY` may be used
for upgrade actions.

List Upgradable checks installed-package state both before and after retrieving daemon targets.
If the installed state changes, the result must be rejected.

A successful empty daemon result means no upgrades were reported. A failed request means upgrade
information is unavailable. These states must remain distinct.

Results are published only after the GTK completion handler accepts them, and only by the refresh
that owns the active refresh ID. Cancelled workers abandon their own refresh rather than invalidating
newer work.

Repository refresh, successful transaction Apply, and detected rpmdb changes invalidate existing
upgrade information. Exact duplicate targets may be collapsed, but conflicting targets for the same
package identity must invalidate the entire result.

The final package transaction still requires a fresh dnf5daemon resolve and user review.

These rules are covered by the daemon upgrade-state unit tests and
`dnf5daemon client lists upgrade targets`.

If upgrade queries become significantly larger, consider `Rpm.list_fd()` rather than relying on
a single D-Bus reply, which has a size limit.

## Repository configuration

Implementation: [repository_service_client.cpp](../src/dnf5daemon_client/repository_service_client.cpp)

Repository enable and disable operations modify privileged system configuration and therefore go
through dnf5daemon.

The client uses:

```text
org.rpm.dnf.v0.rpm.Repo.list
org.rpm.dnf.v0.rpm.Repo.enable_with_options
org.rpm.dnf.v0.rpm.Repo.disable_with_options
```

Repository listing requests `repo_attrs=["id", "name", "enabled"]` and `enable_disable="all"`.

A session with `load_system_repo=false` and `load_available_repos=false` allows repository
configuration to be inspected without loading package metadata.

The client requires valid repository IDs and enabled-state values. Missing fields, incorrect types,
and duplicate repository IDs are rejected. Results are sorted by repository ID rather than relying
on daemon ordering.

Before applying changes, **DNF UI** checks staged repository IDs against a fresh daemon list.
Enable operations run before disable operations, but they remain separate calls rather than one
atomic transaction. Partial failures must therefore be handled.

After writing changes, the client verifies repository state through a new daemon session.
The subsequent libdnf5 Base rebuild is also reported to the user, including failures.

**DNF UI** does not depend on the underlying file or override mechanism used by the daemon to
persist repository settings.

The interface declarations can be checked in the Fedora development image under
`/usr/share/dbus-1/interfaces/`.

Repository parsing and persistence are covered by the repository client tests, including
`Repository client applies changes through fresh daemon sessions`.

## Transaction resolution and Apply

Implementation:

- [transaction_service_client.cpp](../src/dnf5daemon_client/transaction_service_client.cpp)
- [transaction_service_client_dbus.cpp][transaction-client]
- [transaction_service_client_parser.cpp][transaction-parser]
- [transaction_service_client_wait.cpp][transaction-wait]

The daemon resolves package changes through:

```text
Goal.resolve(a{sv}) -> (a(sssa{sv}a{sv})u)
```

The result code determines whether resolution succeeded:

- `0`: success
- `1`: success with warnings
- `2`: failure

Resolver problems are retrieved through `Goal.get_transaction_problems_string()`.

A resolved preview must represent every transaction action that **DNF UI** understands.
Unsupported item types or actions cause preview failure rather than being silently omitted.

### Skipped packages

dnf5daemon can return `Skipped` items with a `reason_skipped` attribute.

Known reasons include `conflict`, `broken_dependency`, and `vendor_change`, as shown in the
[DNF 5.4.4.0 implementation][daemon-goal].

Skipped packages are warnings, not executable transaction actions.
They do not contribute to package action counts or disk-space calculations.

Unknown reasons are preserved in the warning text. Missing reasons use a generic message.

Valid package actions may still be applied when other packages were skipped. A preview containing
only skipped packages has no executable actions and must not be applied.

### Apply and authorization

After explicit user approval, **DNF UI** executes the prepared session through:

```text
Goal.do_transaction(a{sv}) -> ()
```

Both resolve and Apply use `interactive=true`.

dnf5daemon owns privileged package operations and their Polkit authorization.
The GUI remains unprivileged and does not install a separate transaction Polkit policy.

Repository signing-key requests can occur during resolve or Apply. dnf5daemon emits
`repo_key_import_request`, which the client answers through `Repo.confirm_key_with_options(s, b, a{sv})`.

**DNF UI** must obtain explicit user consent before trusting a repository key.
Cancellation during a pending key request must reject the key and allow the worker to finish.

Apply uses the same daemon session that produced the approved preview.

A preview is not a system-wide lock. Other package managers may change the system before Apply,
and dnf5daemon may reject the prepared transaction as a result. The user must then prepare a new preview.

The `do_transaction` call allows a long D-Bus timeout because a Polkit prompt may remain open while
waiting for user input. A short timeout could incorrectly report failure while authorization is pending.

The client receives selected daemon download, verification, and transaction progress signals.
The progress bar shows progress within the current phase rather than estimating one overall
transaction percentage.

The detailed safety and failure-handling policy is documented in [Transaction flow](transactions.md).

Daemon integration tests cover preview, apply, cancellation, authorization failures, disconnects,
and session cleanup. Native testing is required to verify real desktop Polkit prompts and
repository signing-key approval.

## Daemon upgrades and offline transactions

Implementation:

- [transaction_service_client_dbus.cpp][transaction-client]
- [transaction_service_client_offline.cpp][transaction-offline]
- [pending_transaction_apply.cpp](../src/ui/transaction/pending_transaction_apply.cpp)

Updating `dnf5daemon-server` while it is executing a transaction can cause its RPM scriptlets
to restart the service before the transaction has completed.

This is not simply a D-Bus disconnect problem. A scriptlet may wait for a service restart while
the daemon is itself waiting for the scriptlet to finish.

**DNF UI** therefore uses DNF5 offline transactions when the resolved transaction modifies
the daemon package.

### Offline preparation

`Goal.do_transaction` accepts `offline=true`. This prepares the complete resolved transaction
for execution at the next boot rather than installing packages immediately.

The client must not split or modify the approved transaction to work around daemon upgrades.

After preparation, **DNF UI** checks:

```text
Offline.get_status() -> (b, a{sv})
```

The boolean indicates whether a valid DNF transaction is scheduled. The `status` field may still
contain stored update state when the boolean is false.

Preparation is considered successful only when the boolean is true and the state is `ready`.

Known states include `ready`, `download-incomplete`, `download-complete`, and
`transaction-incomplete`. Any nonempty state, including an unknown one, prevents starting
another transaction until the stored state has been handled.

The daemon API does not expose the stored package list, so **DNF UI** does not maintain a separate
copy of prepared transaction items.

### Offline cleanup

Daemon versions expose cleanup options through either:

```text
Offline.clean_with_options(a{sv}) -> (bs)
Offline.clean(a{sv}) -> (bs)
```

The client inspects the active D-Bus interface and calls the supported method with `interactive=true`.
Unsupported signatures are rejected without attempting cleanup.

The [stable D-Bus documentation][daemon-api-stable] also describes a no-argument
`clean_with_options()` variant, so the installed interface must be checked rather than assuming
a particular signature.

Confirmed discard always requests daemon cleanup, even if the reported status is empty.
Empty status can also mean that stored transaction data is incomplete or unreadable.

Cleanup succeeds only after a successful daemon reply and a fresh status check confirming that
no DNF offline state or update boot trigger remains.

DNF cleanup removes its own stored transaction and boot trigger, but does not remove a boot trigger
belonging to another update tool. Cancelling a scheduled reboot is not equivalent to cleaning DNF's
stored transaction.

The behavior follows the [DNF offline command](https://dnf5.readthedocs.io/en/latest/commands/offline.8.html)
and [systemd offline update mechanism][systemd-offline].

### Verification

The `[offline-transaction]` tests use a private D-Bus fixture to verify preparation, status handling,
cleanup signatures, rejection of existing transactions, and protection against repeated submission.

Real-daemon tests use `DNFUI_TEST_DNF5DAEMON_OFFLINE=1` in a disposable environment.
The Docker test harness enables this for preparation and cleanup tests, including malformed stored state.

These tests do not prove that an offline update completes correctly during system boot.

Before changing daemon self-upgrade behavior, use a disposable Fedora VM with systemd to verify that:

1. Updating the daemon prepares the transaction without immediately changing installed packages.
2. Reboot executes the transaction and completes the RPM scriptlets.
3. **DNF UI** works after reboot and no prepared transaction remains.
4. Cleanup and preparation failures leave the system in a recoverable state.

Do not change systemd `KillMode` as a workaround for daemon restarts. Preventing child termination
alone does not resolve a transaction waiting for its own service restart.

See [Transaction flow](transactions.md) for the user-visible behavior.

## GTK and GIO background work

Implementation:

- [package_query_controller.cpp](../src/ui/package_query/package_query_controller.cpp)
- [package_details_controller.cpp](../src/ui/details/package_details_controller.cpp)
- [widgets.cpp](../src/ui/common/widgets.cpp)
- [app.cpp](../src/app.cpp)

**DNF UI** uses `GTask` to move potentially slow backend operations away from the GTK thread.

`g_task_run_in_thread()` executes work on a worker thread. Completion callbacks run in the
thread-default main context where the task was created, allowing GTK-thread completion handlers
to update widgets safely.

This behavior is documented in the [GTask API](https://docs.gtk.org/gio/class.Task.html).

Workers must not directly update GTK widgets. Before accepting a result, completion handlers
must verify that it still belongs to the current request and that any relevant widget,
package selection, or backend generation is still valid.

### Cancellation

`GCancellable` provides cooperative cancellation. It does not terminate worker threads or
automatically interrupt arbitrary libdnf5 operations.

Search and package-list cancellation can interrupt waiting for `BaseManager::acquire_read()`.
Repository downloads can be stopped when libdnf5 invokes a download callback that returns `ABORT`.

Other long-running operations may continue until they reach a cancellation check.

A cancelled worker may therefore finish after a newer request has started. Completion handlers
must safely discard stale results rather than updating the UI.

See [GCancellable](https://docs.gtk.org/gio/class.Cancellable.html) and
[GDBus signal subscriptions](https://docs.gtk.org/gio/method.DBusConnection.signal_subscribe.html)
for the underlying behavior.

Changes to background work should preserve these guarantees and include cancellation or
stale-result tests where applicable.

## Revalidating API assumptions

Changes to Fedora base images, libdnf5, GTK, GLib, GIO, or dnf5daemon may invalidate assumptions
recorded here.

Before accepting changes to an affected integration:

1. Verify relevant declarations and D-Bus signatures against the installed version.
2. Run `make test` and the affected integration tests described in [Testing](testing.md).
3. Test native Polkit authorization and real package transactions when privileged behavior changes.
4. Record manual verification when automated tests cannot reproduce the behavior.

Use `git diff --check` to check documentation and code changes for whitespace errors.

Docker tests are useful for repeatable API and transaction checks, but they do not fully reproduce
native Polkit prompts, systemd service restarts, offline update boot execution, or real-world
repository and network conditions.

When those behaviors change, record the manual verification and its environment in the
pull request or commit message.

[plugin-api]: https://github.com/rpm-software-management/dnf5/blob/5.4.4.0/include/libdnf5/plugin/iplugin.hpp
[daemon-api]: https://dnf5.readthedocs.io/en/latest/dnf_daemon/dnf5daemon_dbus_api.8.html
[daemon-api-stable]: https://dnf5.readthedocs.io/en/stable/dnf_daemon/dnf5daemon_dbus_api.8.html
[daemon-upgrade]: https://github.com/rpm-software-management/dnf5/blob/5.4.4.0/dnf5daemon-server/services/rpm/rpm.cpp#L750-L769
[daemon-goal]: https://github.com/rpm-software-management/dnf5/blob/5.4.4.0/dnf5daemon-server/services/goal/goal.cpp
[systemd-offline]: https://www.freedesktop.org/software/systemd/man/latest/systemd.offline-updates.html
[transaction-client]: ../src/dnf5daemon_client/transaction_service_client_dbus.cpp
[transaction-parser]: ../src/dnf5daemon_client/transaction_service_client_parser.cpp
[transaction-wait]: ../src/dnf5daemon_client/transaction_service_client_wait.cpp
[transaction-offline]: ../src/dnf5daemon_client/transaction_service_client_offline.cpp
