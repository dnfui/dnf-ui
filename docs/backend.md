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

Controller code should use this public API instead of calling libdnf5 directly.

## BaseManager

[src/base_manager.cpp](../src/base_manager.cpp) manages the shared libdnf5 `Base`.

The Base can be in one of three repository states:

- `LIVE_METADATA`: normal repository metadata loaded
- `CACHED_METADATA`: live repository refresh failed, cached metadata loaded
- `INSTALLED_ONLY`: only the local installed package database is available

Most UI queries use serialized read access through `BaseManager::acquire_read`.
The access is serialized because read-only `PackageQuery` work can still touch
shared libdnf5 `Base` internals. The remaining local backend transaction
helpers use write access through `BaseManager::acquire_write`. The normal GUI
preview and apply path goes through dnf5daemon.

Installed-package snapshot refresh uses `BaseManager::acquire_system_only_read`.
That creates a short-lived Base for the local rpm database and does not replace
the shared cached Base used by later package queries.

The Base has a generation counter. When the Base is rebuilt, the generation is
incremented. UI tasks use that value to reject outdated results after refreshes
and transactions.

The shared cached Base also has an id that changes whenever the shared Base is
created, replaced, or dropped. This is useful for code that needs to know when
the exact cached Base object changed, but a Base drop does not mean package rows
are stale.

The package search cache also keeps its own cache epoch. That epoch advances
when the UI explicitly clears cached search rows, even if the backend Base
generation has not changed yet. This keeps older search workers from storing
old rows back into a cache state the UI already invalidated. Search cache
validity depends on the Base generation and this cache epoch, not on the cached
Base id.

## Base cancellation

DNF UI has two places where Stop needs help from the backend:

- repository refresh
- package query workers waiting for the shared Base

Repository refresh passes an atomic cancel flag into `BaseManager::rebuild`.
`BaseManager` installs temporary libdnf download callbacks while loading
repository metadata. When the user presses Stop, the UI sets the flag. The next
download callback that sees the flag returns libdnf's abort status, which tells
libdnf to stop the current transfer.

This is cooperative cancellation. It can stop repository downloads when libdnf
reaches a callback, but it cannot kill an arbitrary libdnf call immediately.

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
- `dnf_backend_get_upgradeable_package_rows_interruptible`
- `dnf_backend_search_package_rows_interruptible`
- `dnf_backend_get_installed_package_rows_by_nevra`
- `dnf_backend_get_available_package_rows_by_nevra`

The browse and search views merge repository candidates with installed-only
packages. The visible result keeps one row for each package name and
architecture pair.

The upgradable backend query returns repository candidates from libdnf5. Before the UI shows the List Upgradable result, it filters those candidates through dnf5daemon's Upgrade All preview. That keeps the visible upgrade list aligned with the transaction service that will actually apply upgrades.

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

The generic Status column is local package metadata. It can say that a newer package exists in enabled repository metadata, but it is not a transaction promise. The List Upgradable view is stricter: it shows only candidates that dnf5daemon also resolved as upgrade items. Transaction preview and apply always go through dnf5daemon.

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

Normal package details use the shared Base. Changelog lookups first read
installed packages from the shared Base because the rpmdb provides that metadata.
Available package changelogs use a temporary Base that requests repository
changelog metadata. The shared Base read lock is released before that temporary
Base is loaded, so transaction preview and apply do not wait behind optional
changelog metadata loading.

## Transactions

[src/dnf_backend/dnf_transaction.cpp](../src/dnf_backend/dnf_transaction.cpp)
contains the backend transaction resolver and apply entry points.

[src/dnf_backend/dnf_transaction_callbacks.cpp](../src/dnf_backend/dnf_transaction_callbacks.cpp)
contains the libdnf download and rpm progress callback adapters.

[src/dnf_backend/dnf_transaction_format.cpp](../src/dnf_backend/dnf_transaction_format.cpp)
contains shared transaction text formatting.

It can resolve previews and apply transactions locally, but the normal GUI apply
path goes through dnf5daemon. The local apply path remains for tests and shared
preview formatting while dnf5daemon coverage is completed.

The preview builder fails closed when libdnf resolves an action that the
preview model cannot represent. That keeps the GUI review step from showing a
partial transaction summary.

Selected package upgrades are sent to dnf5daemon as explicit upgrade specs.
Upgrade All uses dnf5daemon's native upgrade-all behavior instead of building a
local list of upgrade specs.

The GUI should not call local transaction apply directly. Apply should go
through dnf5daemon so the privileged package operation stays outside the GTK
process.

## Internal helpers

[src/dnf_backend/dnf_internal.hpp](../src/dnf_backend/dnf_internal.hpp) and
[src/dnf_backend/dnf_transaction_internal.hpp](../src/dnf_backend/dnf_transaction_internal.hpp)
are shared only by backend implementation files.

It is not a public UI contract. New UI code should include
`dnf_backend.hpp` instead.
