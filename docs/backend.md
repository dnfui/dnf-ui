# Backend Internals

This document explains how DNF UI talks to libdnf5.

For source-backed libdnf5 assumptions, see
[External API assumptions](api-assumptions.md).

## Public Backend Contract

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

[src/base_manager.cpp](../src/base_manager.cpp) owns the shared libdnf5 `Base`.

The Base can be in one of three repository states:

- `LIVE_METADATA`: normal repository metadata loaded
- `CACHED_METADATA`: live repository refresh failed, cached metadata loaded
- `INSTALLED_ONLY`: only the local installed package database is available

Most UI queries use serialized read access through `BaseManager::acquire_read`.
The access is serialized because read-only `PackageQuery` work can still touch
shared libdnf5 `Base` internals. Transaction preview and apply use write access
through `BaseManager::acquire_write`.

The Base has a generation counter. When the Base is rebuilt, the generation is
incremented. UI tasks and search caches use that value to reject outdated
results.

## Query Flow

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

## Installed Snapshot

[src/dnf_backend/dnf_state.cpp](../src/dnf_backend/dnf_state.cpp) owns cached
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

## Package Status

`dnf_backend_get_package_install_state` classifies one visible row as:

- available
- upgradeable
- installed
- local only
- installed newer than repository

Exact installed rows prefer the repository-candidate relation recorded on the
row. Available rows fall back to the installed snapshot so upgrade badges can be
shown without duplicating rows.

## Self Protection

DNF UI blocks removing or reinstalling the package that owns the running GUI
executable from inside the app.

The backend finds the current executable path and asks libdnf5 which installed
package owns it. The UI disables destructive actions for that exact installed
row. The transaction service checks remove and reinstall requests again before
it creates a preview request object.

The relevant functions are:

- `dnf_backend_is_package_self_protected`
- `dnf_backend_is_self_protected_transaction_spec`
- `pending_transaction_validate_request`
- `validate_transaction_request_for_service`

## Package Details

[src/dnf_backend/dnf_details.cpp](../src/dnf_backend/dnf_details.cpp) formats
text for the package details notebook.

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
contains the backend transaction logic.

It resolves a preview before apply, then applies the transaction if the service
authorizes it. Download progress is reported through a callback so the service
can forward progress lines to the GUI.

Upgrade-all requests use libdnf5's all-installed-packages upgrade job instead of
expanding the request into many package specs in the GUI.

The GUI should not call transaction apply directly. Apply should go through the
transaction service so Polkit can authorize it.

## Internal Helpers

[src/dnf_backend/dnf_internal.hpp](../src/dnf_backend/dnf_internal.hpp) is
shared only by backend implementation files.

It is not a public UI contract. New UI code should include
`dnf_backend.hpp` instead.
