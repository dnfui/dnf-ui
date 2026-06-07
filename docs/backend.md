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
shared libdnf5 `Base` internals. Transaction preview and apply use write access
through `BaseManager::acquire_write`.

Installed-package snapshot refresh uses `BaseManager::acquire_system_only_read`.
That creates a short-lived Base for the local rpm database and does not replace
the shared cached Base used by later package queries.

The Base has a generation counter. When the Base is rebuilt, the generation is
incremented. UI tasks use that value to reject outdated results after refreshes
and transactions.

The shared cached Base also has an id that changes whenever the shared Base is
created, replaced, or dropped. The package search cache uses it so rows from one
cached Base are not reused after that Base was discarded and recreated under the
same generation.

The package search cache also keeps its own cache epoch. That epoch advances
when the UI explicitly clears cached search rows, even if the backend Base
generation has not changed yet. This keeps older search workers from storing
old rows back into a cache state the UI already invalidated.

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
upgradable repository candidate. Upgrade actions use the visible update NEVRA,
while remove and reinstall use the currently installed NEVRA for the same
package name and architecture.

If the visible row is the installed package and its status is update available,
the query row carries the matching available upgrade NEVRA. The UI uses that
stored package ID for the upgrade action without doing another package query
from the GTK thread.

The snapshot is updated only after a complete installed-package scan. Cancelled
queries do not publish partial installed state.

## Package status

`dnf_backend_get_package_install_state` classifies one visible row as:

- available
- upgradeable
- installed
- local only
- installed newer than repository

Exact installed rows prefer the repository-candidate relation recorded on the
row. Available rows fall back to the installed snapshot so upgrade badges can be
shown without duplicating rows.

## Self protection

DNF UI blocks modifying the package that owns the running GUI executable from
inside the app.

The backend finds the current executable path and asks libdnf5 which installed
package owns it. The UI disables package changes for that exact installed row.
The pending transaction request is checked again before it is sent to
dnf5daemon.

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
If the selected package is not installed, the lookup uses a short-lived temporary
Base that requests repo `other` metadata, so normal list and search queries do
not keep changelog metadata resident.

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

Upgrade-all requests in the GUI build explicit upgrade specs and send them to
dnf5daemon.

The GUI should not call local transaction apply directly. Apply should go
through dnf5daemon so the privileged package operation stays outside the GTK
process.

## Internal helpers

[src/dnf_backend/dnf_internal.hpp](../src/dnf_backend/dnf_internal.hpp) and
[src/dnf_backend/dnf_transaction_internal.hpp](../src/dnf_backend/dnf_transaction_internal.hpp)
are shared only by backend implementation files.

It is not a public UI contract. New UI code should include
`dnf_backend.hpp` instead.
