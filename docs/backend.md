# Backend internals

This document explains how DNF UI talks to libdnf5.

For source-backed libdnf5 assumptions, see [External API assumptions](api-assumptions.md).

## Public backend contract

The UI uses [src/dnf_backend/dnf_backend.hpp](../src/dnf_backend/dnf_backend.hpp).

That header is the app-facing package-data contract. It keeps libdnf5 package
types out of the GTK controller layer by exposing small value types:

- `PackageRow`
- `PackageInstallReason`
- `PackageInstallState`
- `PackageRepoCandidateRelation`
- `DnfBackendSearchOptions`
- `TransactionHistoryAction`
- `TransactionHistoryPackageRow`
- `TransactionHistoryCursor`
- `TransactionHistoryResultFilter`
- `TransactionHistoryFilter`
- `TransactionHistoryPage`

Controller code should use this public API for package rows, package details, installed-state answers,
transaction history, and RPM comparisons. Table sorting also goes through this facade so UI code does not
include libdnf5 headers.

`BaseManager` is the separate backend lifecycle coordinator. Async controllers use it directly when they need
Base generation checks, startup warmup, repository rebuilds, or memory release after package queries. That is
an intentional boundary: controllers may coordinate Base lifetime through `BaseManager`, but package data and
RPM rules still go through `dnf_backend.hpp`.

## BaseManager

[src/dnf_backend/base_manager.cpp](../src/dnf_backend/base_manager.cpp) manages the shared libdnf5 `Base`.

The Base can be in one of three repository states:

- `LIVE_METADATA`: normal repository metadata loaded
- `CACHED_METADATA`: live repository refresh failed, cached metadata loaded
- `INSTALLED_ONLY`: only the local installed package database is available

Most UI queries use serialized read access through `BaseManager::acquire_read`. The access is serialized
because read-only `PackageQuery` work can still touch shared libdnf5 `Base` internals. Package transaction
preview and apply work goes through dnf5daemon instead of a local libdnf transaction path.

Installed-package snapshot refresh uses `BaseManager::acquire_system_only_read_after_dropping_cached_base`.
While holding the BaseManager lock, it drops the shared cached Base and constructs a short-lived system-only
Base for the local rpm database scan. This prevents another worker from recreating the shared Base between
cache invalidation and the installed-package scan.

The temporary system-only Base is destroyed before the manager lock is released. The shared Base remains empty
after the scan and is rebuilt on demand by the next normal package query. This cache invalidation does not
increment the Base generation because repository metadata was not refreshed. The separate
`acquire_system_only_read` path remains for temporary local rpm database reads that must not invalidate the
shared Base.

The Base has a generation counter. When the Base is rebuilt, the generation is incremented. UI tasks use that
value to reject outdated results after refreshes and transactions.

Provides searches use a temporary Base that requests DNF5's optional `filelists` metadata. This finds files
belonging to uninstalled packages, including paths not listed in primary metadata. The temporary Base uses the
same serialization lock and is released when the search finishes. It does not replace or discard the normal
cached Base. Ordinary package queries, startup, and repository refresh keep their existing metadata
requirements and offline fallback behavior. Provides retries with file lists from its configured cache, then
the system cache if live loading fails, and reports an error when those lists are unavailable rather than
silently returning incomplete results. Downloading and loading file lists adds work only to Provides searches
unless the user's DNF configuration already requests them. Repositories containing queryable packages must
actually supply file-list metadata, requesting the optional metadata alone is not sufficient.

## Base lifetime and search cache

The shared Base is heavyweight libdnf5 state. Normal package-query workers drop it when they finish so
repository metadata does not stay resident just because the user ran one query. Search, List Installed, List
Packages, List Upgradable, and exact selected-package reload workers use this policy. If a worker was
cancelled, it skips the drop so Stop does not block again while trying to release cached metadata.

Dropping the shared Base is a memory policy. It does not mean that repository metadata was refreshed, and it
does not increment the Base generation. The next normal query rebuilds the Base on demand and continues using
the current generation unless a real rebuild publishes a replacement Base.

Search cache entries are lightweight snapshots of completed Search results. They store copied `PackageRow`
values, not libdnf5 package objects. They can therefore survive shared Base destruction. Base object lifetime
is not the search cache lifetime.

Ordinary searches and Provides searches each retain up to three entries with independent eviction. They share
the same generation and invalidation epoch, so refresh and Clear Cache invalidate both modes.

Search cache validity depends on two values:

- the Base generation recorded when the rows were produced
- the search-cache epoch recorded when the rows were produced

The Base generation changes when a repository refresh or transaction follow-up publishes a rebuilt Base. The
search-cache epoch changes when the UI explicitly clears cached search rows. Repository refresh, transaction
completion, periodic installed-state refresh, and Clear Cache all clear the search cache and advance that
epoch.

The cache epoch exists because the UI can invalidate cached rows without a Base generation change. It also
prevents an older search worker from storing rows back into the cache after the UI has already cleared it.

All-version searches bypass the search cache because they can contain many more rows than the compact Latest
only view.

## Repository refresh

The manual Refresh Repositories button refreshes both package-management paths used by the app.

DNF UI refreshes dnf5daemon first because transaction previews and apply work go through the daemon. It asks
the daemon to expire its metadata cache, reset the daemon session, and read all repositories. After that
succeeds, the UI force-refreshes its own libdnf5 Base because package views and package details are still
built from libdnf5.

The refresh session is opened without preloading installed packages or available repositories. The refresh
code must expire metadata before asking dnf5daemon to load repositories again.

After refresh, normal package views are reloaded from the new Base. If List Upgradable is visible, the table
is cleared instead of left on screen, because those rows came from the previous daemon upgrade snapshot. The
app asks the user to press List Upgradable again so a fresh dnf5daemon upgrade list is loaded explicitly.

If the daemon cache directory does not exist yet, the clean step is treated as already clean. The refresh
still resets the daemon session and loads repositories.

This order matters because package previews and applies use dnf5daemon. If the UI refreshed only its own
libdnf5 Base, the table could show package metadata that dnf5daemon does not resolve yet. If only dnf5daemon
was refreshed, package views could still show stale libdnf5 rows.

## Base cancellation

DNF UI has two places where Stop needs help from the backend:

- repository refresh
- package query workers waiting for the shared Base

Repository refresh uses two cancellation paths. It passes a `GCancellable` to the dnf5daemon D-Bus calls, and
it passes an atomic cancel flag into `BaseManager::rebuild`. When the user presses Stop, the UI asks both the
daemon call and the later UI Base rebuild to stop. The same cancellation path is used when the main window is
closed during repository refresh.

This is cooperative cancellation. It cannot kill arbitrary libdnf or D-Bus work immediately, but stopped
refresh work must not publish a partial replacement Base.

Package query workers use `GCancellable`, because that is the normal GLib task cancellation type.
`dnf_query.cpp` converts that into the same atomic flag before calling `BaseManager::acquire_read`. This lets
a stopped search or package list task give up while it is waiting for the shared Base lock or while Base
initialization is starting.

## Query flow

[src/dnf_backend/dnf_query.cpp](../src/dnf_backend/dnf_query.cpp) builds the package rows used by search,
browse, and installed-list views.

The main query paths are:

- `dnf_backend_get_installed_package_rows_interruptible`
- `dnf_backend_get_browse_package_rows_interruptible`
- `dnf_backend_get_available_package_metadata_by_nevras_interruptible`
- `dnf_backend_search_package_rows_interruptible`
- `dnf_backend_get_installed_package_rows_by_nevra`
- `dnf_backend_get_available_package_rows_by_nevra`

The browse and search views merge repository candidates with installed-only packages. The visible result keeps
one row for each package name and architecture pair when Latest only is enabled. When Latest only is disabled,
the visible result keeps one row per exact available NEVRA and adds missing exact installed NEVRAs. Duplicate
repository copies of the same exact NEVRA are hidden.

When an exact installed NEVRA is also present in an enabled repository, the all-version view keeps one row for
that NEVRA and annotates it with installed state and its newest visible repository candidate.

Normal search is substring based. If the search term contains `*` or `?`, normal search treats it as a
wildcard pattern. Exact search remains literal.

The GTK controller captures the description, exact-match, and Latest only checkboxes once for each search
request. That same option snapshot is used for the search cache key and passed to the backend worker, so a
queued search does not depend on later checkbox changes. Changing Latest only does not reload the table by
itself. It affects the next Search or List Packages request.

The List Upgradable view uses dnf5daemon to decide which upgrades exist. The worker refreshes
installed-package state, loads the daemon upgrade targets, asks libdnf5 only for matching package metadata,
and then refreshes installed-package state again.

If installed state changed while the daemon result was being loaded, the result is rejected and the user must
reload List Upgradable. The GTK completion stores the daemon targets in the shared daemon upgrade snapshot
only when it accepts the matching table rows. Missing metadata does not hide a daemon-reported upgrade. In
that case the table keeps a basic row built from the daemon target.

This keeps the list honest: libdnf5 can add metadata to daemon-reported upgrade rows. It no longer decides
which rows appear in List Upgradable.

## Installed snapshot

[src/dnf_backend/dnf_state.cpp](../src/dnf_backend/dnf_state.cpp) keeps cached state about installed packages.

It stores:

- exact installed NEVRAs
- installed rows keyed by package name and architecture
- package names that own the running GUI executable

The installed snapshot lets the UI classify package rows without doing a fresh libdnf5 query for every table
update.

When one UI decision needs several installed-state answers for the same row, the backend resolves them
together from one snapshot.

The same snapshot also lets the UI resolve the installed package behind an upgradable repository candidate.
Upgrade actions keep the visible update NEVRA for UI navigation, but send a package name and architecture spec
to dnf5daemon. Downgrade actions send the selected exact older NEVRA. Remove and reinstall use the exact
installed NEVRA.

When the visible row is installed and has a newer repository candidate, the query row stores the candidate
NEVRA. The UI uses that stored package ID for the upgrade action without doing another package query from the
GTK thread.

Only the newest available row for a package name and architecture can become a normal upgrade action.
Intermediate newer rows remain visible for inspection, but they cannot be marked as upgrades because
dnf5daemon upgrades by package spec and may resolve a newer candidate than the selected row.

In all-version views, an exact installed row with a newest available candidate can start or cancel the same
upgrade action as the newest available row. Pending upgrade status still stays on the exact available target
row, so the installed row does not look like a separate pending upgrade.

The snapshot is updated only after a complete installed-package scan. Cancelled queries do not publish partial
installed state.

## Package status

`dnf_backend_resolve_installed_package` classifies one visible row as:

- available
- upgradeable
- older in repository
- installed
- local only
- installed newer than repository

Exact installed rows prefer the repository-candidate relation recorded on the row. Available rows fall back to
the installed snapshot so the table can show when repository metadata contains a newer or older candidate
without duplicating rows. The same resolved value also carries exact-installed state and the installed row for
the same package name and architecture.

When several EVRs of the same package name and architecture are installed, upgrade availability is based on
the newest installed EVR. Older installed rows remain installed rows even when a newer repository candidate
exists, because install-only packages such as kernels can be installed side by side. Package rows also carry
DNF's install-only classification so the pending transaction model can allow several exact install actions for
install-only packages without hardcoding kernel package names. Installed rows carry whether their exact NEVRA
is still present in enabled repository metadata. The UI uses that value to decide whether Reinstall can be
offered without running a repository query from GTK code.

The generic Status column is local package metadata. It can say that a newer or older package exists in
enabled repository metadata, but it is not a transaction promise. The List Upgradable view is stricter: it
shows the current daemon upgrade targets and keeps the daemon target on the table row so the update columns
and Mark for Upgrade use the same snapshot. Transaction preview and apply always go through dnf5daemon.

## Self protection

The backend finds the current executable path and asks libdnf5 which installed package owns it. Transaction
safety policy is documented in [transactions.md](transactions.md).

The discovered package owner is treated as a process-lifetime safety boundary. If a later lookup cannot
rediscover the package, for example after the running executable has been replaced by an upgrade, the
established protected package name is kept instead of being cleared.

The relevant functions are:

- `dnf_backend_is_self_protected_package_name`
- `dnf_backend_is_self_protected_transaction_spec`
- `pending_transaction_validate_request`

## Package details

[src/dnf_backend/dnf_details.cpp](../src/dnf_backend/dnf_details.cpp) formats text for the package details
panel.

It provides:

- package summary and metadata
- installed file list
- dependencies
- changelog

These helpers perform read-only libdnf5 queries and do not mutate the installed snapshot.

When the selected row has a daemon upgrade target, the details controller keeps the visible row ID separate
from the package ID used for installed details. Info, files, and dependencies use the installed counterpart
when it is known. Changelog uses the exact daemon upgrade target because that row represents the package
version the user is deciding whether to install. The Info tab uses the attached daemon target for the
upgradable version line instead of asking libdnf5 to choose a separate candidate.

Newest upgrade rows in compact Search and List Packages views use installed context for Info, Files, and
Dependencies so they match the installed version and Update Version columns. Their changelog uses the
available update NEVRA stored on the row. List Upgradable rows use installed context for Info, Files, and
Dependencies, while their changelog uses the daemon-selected upgrade target.

When Latest only is disabled, each available row uses selected-version context, including the globally newest
available version. This makes Info, Dependencies, and Changelog describe the exact selected NEVRA. Files
remains available only when that exact NEVRA is installed.

## Transactions

DNF UI does not keep a local libdnf transaction apply path. Transaction preview and apply go through
dnf5daemon so privileged package changes stay outside the GTK process.

Selected package upgrades are sent to dnf5daemon as explicit upgrade specs. Selected downgrades are sent as
exact NEVRA specs through dnf5daemon's downgrade method. Upgrade All uses dnf5daemon's native upgrade-all
behavior instead of building a local list of upgrade specs.

The dnf5daemon client reads daemon replies and uses
[src/dnf5daemon_client/transaction_service_client_parser.cpp](../src/dnf5daemon_client/transaction_service_client_parser.cpp)
to add supported package items to `TransactionPreview`.
The shared preview model is this header:
[src/transaction/transaction_preview.hpp](../src/transaction/transaction_preview.hpp). It is not part of the
libdnf backend facade. Preview parsing fails closed when a daemon transaction item cannot be represented by
the UI preview model.

## Internal helpers

[src/dnf_backend/dnf_internal.hpp](../src/dnf_backend/dnf_internal.hpp) is shared by backend implementation
files and backend tests.

It is not a public UI contract. New UI code should include `dnf_backend.hpp` instead.
