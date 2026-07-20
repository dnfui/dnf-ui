# Backend internals

This document explains how DNF UI talks to libdnf5.

For source-backed libdnf5 assumptions, see
[External API assumptions](api-assumptions.md).

## Public backend contract

The UI uses [src/dnf_backend/dnf_backend.hpp](../src/dnf_backend/dnf_backend.hpp).

That header is the app-facing contract. It keeps libdnf5 types out of the GTK
controller layer by exposing small value types:

- `PackageRow`
- `PackageInstallReason`
- `PackageInstallState`
- `PackageRepoCandidateRelation`
- `DnfBackendSearchOptions`
- `TransactionPreview`
- `TransactionHistoryAction`
- `TransactionHistoryPackageRow`
- `TransactionHistoryCursor`
- `TransactionHistoryResultFilter`
- `TransactionHistoryFilter`
- `TransactionHistoryPage`

Controller code should use this public API instead of calling libdnf5 directly.

## BaseManager

[src/dnf_backend/base_manager.cpp](../src/dnf_backend/base_manager.cpp) manages the shared libdnf5 `Base`.

The Base can be in one of three repository states:

- `LIVE_METADATA`: normal repository metadata loaded
- `CACHED_METADATA`: live repository refresh failed, cached metadata loaded
- `INSTALLED_ONLY`: only the local installed package database is available

Most UI queries use serialized read access through `BaseManager::acquire_read`.
The access is serialized because read-only `PackageQuery` work can still touch
shared libdnf5 `Base` internals. Package transaction preview and apply work
goes through dnf5daemon instead of a local libdnf transaction path.

Installed-package snapshot refresh uses `BaseManager::acquire_system_only_read`.
That creates a short-lived Base for the local rpm database and does not replace
the shared cached Base used by later package queries.

The Base has a generation counter. When the Base is rebuilt, the generation is
incremented. UI tasks use that value to reject outdated results after refreshes
and transactions.

The package search cache also keeps its own cache epoch. That epoch advances
when the UI explicitly clears cached search rows, even if the backend Base
generation has not changed yet. This keeps older search workers from storing
old rows back into a cache state the UI already invalidated. Search cache
validity depends on the Base generation and this cache epoch.

## Repository refresh

The manual Refresh Repositories button refreshes both package-management paths
used by the app.

DNF UI refreshes dnf5daemon first because transaction previews and apply work go
through the daemon. It asks the daemon to expire its metadata cache, reset the
daemon session, and read all repositories. After that succeeds, the UI
force-refreshes its own libdnf5 Base because package views and package details
are still built from libdnf5.

The refresh session is opened without preloading installed packages or available
repositories. The refresh code must expire metadata before asking dnf5daemon to
load repositories again.

After refresh, normal package views are reloaded from the new Base. If
List Upgradable is visible, the table is cleared instead of left on screen,
because those rows came from the previous daemon upgrade snapshot. The app asks
the user to press List Upgradable again so a fresh dnf5daemon upgrade list is
loaded explicitly.

If the daemon cache directory does not exist yet, the clean step is treated as
already clean. The refresh still resets the daemon session and loads repositories.

This order matters because package previews and applies use dnf5daemon. If the
UI refreshed only its own libdnf5 Base, the table could show package metadata
that dnf5daemon does not resolve yet. If only dnf5daemon was refreshed, package
views could still show stale libdnf5 rows.

## Base cancellation

DNF UI has two places where Stop needs help from the backend:

- repository refresh
- package query workers waiting for the shared Base

Repository refresh uses two cancellation paths. It passes a `GCancellable` to
the dnf5daemon D-Bus calls, and it passes an atomic cancel flag into
`BaseManager::rebuild`. When the user presses Stop, the UI asks both the daemon
call and the later UI Base rebuild to stop. The same cancellation path is used
when the main window is closed during repository refresh.

This is cooperative cancellation. It cannot kill arbitrary libdnf or D-Bus work
immediately, but stopped refresh work must not publish a partial replacement
Base.

Package query workers use `GCancellable`, because that is the normal GLib task
cancellation type. `dnf_query.cpp` converts that into the same atomic flag before
calling `BaseManager::acquire_read`. This lets a stopped search or package list
task give up while it is waiting for the shared Base lock or while Base
initialization is starting.

## Query flow

[src/dnf_backend/dnf_query.cpp](../src/dnf_backend/dnf_query.cpp) builds the
package rows used by search, browse, and installed-list views.

The main query paths are:

- `dnf_backend_get_installed_package_rows_interruptible`
- `dnf_backend_get_browse_package_rows_interruptible`
- `dnf_backend_get_available_package_metadata_by_nevras_interruptible`
- `dnf_backend_search_package_rows_interruptible`
- `dnf_backend_get_installed_package_rows_by_nevra`
- `dnf_backend_get_available_package_rows_by_nevra`

The browse and search views merge repository candidates with installed-only
packages. The visible result keeps one row for each package name and
architecture pair.

Normal search is substring based. If the search term contains `*` or `?`, normal search treats it as a wildcard pattern. Exact search remains literal.

The List Upgradable view uses dnf5daemon to decide which upgrades exist. The worker refreshes installed-package state, loads the daemon upgrade targets, asks libdnf5 only for matching package metadata, and then refreshes installed-package state again. If installed state changed while the daemon result was being loaded, the result is rejected and the user must reload List Upgradable. The GTK completion stores the daemon targets in the shared daemon upgrade snapshot only when it accepts the matching table rows. Missing metadata does not hide a daemon-reported upgrade. In that case the table keeps a basic row built from the daemon target.

This keeps the list honest: libdnf5 can add metadata to daemon-reported upgrade rows, but it no longer decides which rows appear in List Upgradable.

## Installed snapshot

[src/dnf_backend/dnf_state.cpp](../src/dnf_backend/dnf_state.cpp) keeps cached
state about installed packages.

It stores:

- exact installed NEVRAs
- installed rows keyed by package name and architecture
- current backend search options
- package names that own the running GUI executable

The installed snapshot lets the UI classify package rows without doing a fresh
libdnf5 query for every table update.

The same snapshot also lets the UI resolve the installed package behind an
upgradable repository candidate. Upgrade actions keep the visible update NEVRA
for UI navigation, but send a package name and architecture spec to
dnf5daemon. Remove and reinstall use the currently installed NEVRA for the same
package name and architecture.

If the visible row is the installed package and a newer repository candidate was found, the query row carries the matching available candidate NEVRA. The UI uses that stored package ID for the upgrade action without doing another package query from the GTK thread.

The snapshot is updated only after a complete installed-package scan. Cancelled
queries do not publish partial installed state.

## Package status

`dnf_backend_get_package_install_state` classifies one visible row as:

- available
- upgradeable
- installed
- local only
- installed newer than repository

Exact installed rows prefer the repository-candidate relation recorded on the row. Available rows fall back to the installed snapshot so the table can show when repository metadata contains a newer candidate without duplicating rows.

The generic Status column is local package metadata. It can say that a newer package exists in enabled repository metadata, but it is not a transaction promise. The List Upgradable view is stricter: it shows the current daemon upgrade targets and keeps the daemon target on the table row so the update columns and Mark for Upgrade use the same snapshot. Transaction preview and apply always go through dnf5daemon.

## Self protection

DNF UI blocks direct remove and reinstall requests for the package that owns the
running GUI executable from inside the app. Normal upgrades are allowed because
users must be able to update DNF UI and dnf5daemon from the package manager UI.

The backend finds the current executable path and asks libdnf5 which installed
package owns it. The UI disables package changes for that exact installed row.
The pending transaction request is checked again before it is sent to
dnf5daemon. After dnf5daemon resolves the preview, DNF UI rejects any transaction
that would remove or replace the running app package.

The relevant functions are:

- `dnf_backend_is_package_self_protected`
- `dnf_backend_any_self_protected_package_label`
- `dnf_backend_is_self_protected_transaction_spec`
- `pending_transaction_validate_request`

## Package details

[src/dnf_backend/dnf_details.cpp](../src/dnf_backend/dnf_details.cpp) formats
text for the package details panel.

It provides:

- package summary and metadata
- installed file list
- dependencies
- changelog

These helpers perform read-only libdnf5 queries and do not mutate the installed
snapshot.

When the selected row has a daemon upgrade target, the details controller keeps
the visible row ID separate from the package ID used for details. Info, files,
dependencies, and changelog use the installed counterpart when it is known. The
Info tab uses the attached daemon target for the upgradable version line instead
of asking libdnf5 to choose a separate candidate.

## Transactions

DNF UI does not keep a local libdnf transaction apply path. Transaction preview
and apply go through dnf5daemon so privileged package changes stay outside the
GTK process.

Selected package upgrades are sent to dnf5daemon as explicit upgrade specs.
Upgrade All uses dnf5daemon's native upgrade-all behavior instead of building a
local list of upgrade specs.

The dnf5daemon client builds `TransactionPreview` values from daemon replies and
fails closed when a daemon transaction item cannot be represented by the UI
preview model.

## Internal helpers

[src/dnf_backend/dnf_internal.hpp](../src/dnf_backend/dnf_internal.hpp) is shared
only by backend implementation files.

It is not a public UI contract. New UI code should include
`dnf_backend.hpp` instead.
