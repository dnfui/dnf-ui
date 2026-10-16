# UI internals

This document explains how the GTK user interface is organized.

For source-backed GTK and GIO assumptions, see
[External API assumptions](api-assumptions.md).

## Main idea

`main_window_layout.cpp` builds the widget tree. `main_window.cpp` creates
shared widget state and connects behavior. Controller files own the behavior
behind each part of the window.

The shared widget state lives in [src/ui/common/widgets.hpp](../src/ui/common/widgets.hpp).
Controller files receive a `MainWindowUiState` pointer and use it to update the
parts of the window they own.

This keeps widget construction, package query behavior, package details,
repository refresh behavior, and pending transaction behavior in separate files.

The UI source tree is grouped by the part of the window it owns:

- `src/ui/window` for the main window, layout, and menu
- `src/ui/package_query` for search, package listing, query cache, and Stop handling
- `src/ui/package_table` for the package table model, columns, status, sorting, and context menu
- `src/ui/details` for the package details panel
- `src/ui/transaction` for marked actions, preview, apply, review dialogs, and progress
- `src/ui/refresh` for manual repository refresh
- `src/ui/repository` for the repository list and enabled-state changes
- `src/ui/common` for shared widget state and small GTK helpers

## Window construction

[src/ui/window/main_window.cpp](../src/ui/window/main_window.cpp) creates the main window.
[src/ui/window/main_window_layout.cpp](../src/ui/window/main_window_layout.cpp) builds the
GTK widget tree used by that window.

The layout file is responsible for:

- creating the top menu
- creating search and list buttons
- creating the package table
- creating the package details panel
- creating the pending actions tab
- creating transaction action buttons

The main window file is responsible for:

- connecting GTK signals to controller callbacks
- saving window size and pane positions

It should not contain package query logic or transaction apply logic. Those
belong in the controller files.

## Shared widget state

[src/ui/common/widgets.hpp](../src/ui/common/widgets.hpp) groups the widget pointers into
smaller structs:

- `PackageQueryWidgets` for search controls and status
- `PackageResultsWidgets` for the package table and details panel
- `PendingTransactionWidgets` for pending action controls
- `PendingTransactionState` for marked actions and preview state
- `MainWindowState` for window-level state
- `MainWindowUiState` as the top-level shared state passed to controllers

This state is not meant to hide ownership. It is a practical place to store
GTK pointers that several controllers need.

## Controller files

### Package query controller

[src/ui/package_query/package_query_controller.cpp](../src/ui/package_query/package_query_controller.cpp)
handles the public GTK callbacks for package list workflows:

- list installed packages
- browse available and installed packages together
- list installed packages that have available updates
- search packages
- restore a search from history
- clear the package list
- reload the current view after package state changes

The supporting package query files keep the slower and more stateful parts out
of the public callback file:

- [src/ui/package_query/package_query_controls.cpp](../src/ui/package_query/package_query_controls.cpp)
  handles active request state, Stop button handling, cancellation, and refresh
  completion.
- [src/ui/package_query/package_query_tasks.cpp](../src/ui/package_query/package_query_tasks.cpp)
  contains the `GTask` workers and completion handlers for package queries.
- [package_query_controller_internal.hpp](../src/ui/package_query/package_query_controller_internal.hpp)
  declares the shared functions used by those files.

Long-running package queries run on worker threads through `GTask`. Completion
callbacks run on the GTK thread before they update widgets.

The search-mode selector switches between normal package-text search and **Provides** lookup.
Provides accepts an RPM capability (for example `pkgconfig(openssl)`), an absolute file path
(for example `/usr/bin/bash`), or a filename (for example `zshrc`). Filename searches also accept
wildcards. Matches come from installed packages
and enabled repository metadata and use the existing package table, details, and transaction actions.
Description and Exact match checkboxes apply only to package-text searches. Both search modes
share the same cancellation, cache, and view refresh behavior.

The Latest only checkbox is enabled by default. It keeps Search and List
Packages in the compact one-row-per-package view. When disabled, those two
queries show exact package versions. Older repository versions can then be
inspected and marked for downgrade. Intermediate newer versions remain visible
for inspection, but only the newest available version can be marked for upgrade.
All-version views use separate Status labels for the installed row and the available update row.
This keeps the rows from looking like the same action target.
List Installed and List Upgradable do not use this checkbox.

The bottom bar shows the visible row count on the left and the last completed
package query time on the right.

After startup, DNF UI quietly checks dnf5daemon's read-only upgrade target list.
When that check succeeds, the List Upgradable button shows the current number
of daemon-reported upgrades. Pressing the button still loads the full table.
After Refresh Repositories completes with live metadata, DNF UI refreshes that
count quietly again.

Search result caching uses this file:
[src/ui/package_query/package_query_cache.cpp](../src/ui/package_query/package_query_cache.cpp).
The cache is tied to the current backend Base generation and a cache epoch kept
by the query cache layer. Repository refreshes, transaction follow-up refreshes,
and installed-state refreshes clear cached search rows and advance that epoch,
so older search workers cannot repopulate the cache with rows the UI has already
invalidated. Dropping the cached Base to save memory does not invalidate search
rows by itself. All-version searches are not stored in this cache because they
can be much larger than the normal compact result.

[src/ui/refresh/repository_refresh_controller.cpp](../src/ui/refresh/repository_refresh_controller.cpp)
owns the Refresh Repositories button workflow. It refreshes dnf5daemon metadata,
rebuilds the libdnf5 Base, updates the lower-right progress text, and clears
stale upgradable rows after repository metadata changes. The same workflow can
be started with F5.

`View -> Repositories...` or Ctrl+Shift+R opens a repository list window backed by dnf5daemon.
The implementation lives in
[src/ui/repository/repository_view.cpp](../src/ui/repository/repository_view.cpp).
It lists repository ID, name, and enabled state on a worker thread so opening
the window does not block the main UI. Checkbox changes are kept pending until
the user presses Apply. The user can clear those pending repository changes
without applying them. Reload is disabled while repository changes are pending,
so staged changes are not discarded silently. Before changing repository state,
the window shows a short review dialog with the repositories that will be enabled or disabled.
The window applies repository enable and disable changes through dnf5daemon,
rebuilds the main package backend with the new repository configuration, and
then reads the final repository state through a fresh daemon session. Apply is
not cancellable once it starts, and the repository window stays open until it
finishes. Before writing, Apply checks the current daemon repository state and
sends only changes that are still needed. Apply waits for package queries,
repository refresh, and transaction preview or apply work to finish before
changing repository state. After Apply, the UI reports whether package data was
reloaded from live metadata, cached metadata, or installed packages only.

### Package details controller

[src/ui/details/package_details_controller.cpp](../src/ui/details/package_details_controller.cpp)
updates the details pane for the selected package.

It updates:

- selected package status
- package details text
- installed file list, loaded only when the Files tab is opened
- dependencies, loaded only when the Dependencies tab is opened
- changelog, loaded only when the Changelog tab is opened
- install, remove, and reinstall button sensitivity

Details are loaded in the background. Selecting a package loads the Info tab.
Files, dependencies, and changelog data are loaded when their tabs are opened.
The controller records the selected NEVRA and backend generation when each task
starts. If the selected package changes or the backend generation changes, the
old result is ignored. Changelog loading can require extra repository metadata
for available packages, so it remains separate from the normal Info load.
Rows with an upgrade target keep a separate changelog target so the Changelog
tab can show the update package while Info, Files, and Dependencies still use
the installed package context.
When the Status column is hidden, the Info tab keeps Status as a live label
outside the fetched package metadata text, so pending transaction changes do not
leave stale Status text in the details panel.

### Package table view

[src/ui/package_table/package_table_view.cpp](../src/ui/package_table/package_table_view.cpp) builds the
package table, including column setup, selection, and status refresh.

The table columns can be shown or hidden from `View -> Columns`, and the same
menu can restore the default column set. The setting is stored in `dnfui.conf`
as `package_table_hidden_columns`, using stable column ids so new default-visible
columns can be added without hiding them for existing users. Older
`package_table_columns` settings are migrated when they are read. Saving
preferences is best-effort. The app keeps running if the config file cannot be
created or updated.

`File -> Export Package List...` or Ctrl+E writes the currently visible package
table rows to a CSV file. It exports the table model that is already shown to
the user instead of running another backend query. CSV fields that could be
read as spreadsheet formulas are prefixed before normal CSV escaping.

[src/ui/package_table/package_table_columns.cpp](../src/ui/package_table/package_table_columns.cpp) owns the
package table column definitions, stable column ids, saved visibility settings,
and config migration.

[src/ui/package_table/package_table_model.cpp](../src/ui/package_table/package_table_model.cpp) contains the
GTK object wrapper used to store package rows and their table display values.

[src/ui/package_table/package_table_sort.cpp](../src/ui/package_table/package_table_sort.cpp) contains package
table cell text lookup and sorting rules. Version columns sort by stored RPM
epoch and version. Release columns sort by stored RPM release text.

[src/ui/package_table/package_table_export.cpp](../src/ui/package_table/package_table_export.cpp) exports the
current table rows to CSV.
[src/ui/package_table/package_table_export_csv.cpp](../src/ui/package_table/package_table_export_csv.cpp) formats the
CSV text, including spreadsheet formula protection, and is tested without
opening a GTK file dialog.

[src/ui/package_table/package_table_status.cpp](../src/ui/package_table/package_table_status.cpp) keeps the
status text and CSS classes separate from table construction.

[src/ui/package_table/package_table_context_menu.cpp](../src/ui/package_table/package_table_context_menu.cpp)
builds right-click actions for package rows.

### Transaction history

`Package -> Transaction History...` or Ctrl+Shift+H opens a read-only window
backed by libdnf5 transaction history. It lists recent package changes and lets
the user filter them by package, action, result, date range, repository,
architecture, or command text.

The history window implementation is this file:
[src/ui/history/transaction_history_view.cpp](../src/ui/history/transaction_history_view.cpp).
It loads history on a worker thread and displays value objects from the backend
instead of libdnf5 objects. It shows 100 package changes per page and supports
Newer, Older, and direct page navigation without creating an unbounded GTK list.
The page control does not show a total page count, because that would require
scanning the full matching history before the window can be used. Filters are
applied by the backend before the page is returned, so a search looks through
the available history instead of only the rows currently shown on screen. Filter
changes are applied when the user presses Search or presses Enter in a filter
entry. This avoids starting a backend history scan for every typed character.
The action filter uses checkboxes so the user can include one action, several
actions, or all actions in the same history search.
When the history window is focused, Ctrl+F focuses the package filter and Ctrl+W closes the window.
If a package transaction summary is open, the history window remains usable.
If a package transaction is being applied, the history window stays open and can be closed.
Its controls are disabled until apply finishes.
The navigation row shows how long the last completed history search or page load took.
The feature is intentionally read-only. It does not offer rollback, replay, or
undo actions.

### Pending transaction controller

[src/ui/transaction/pending_transaction_controller.cpp](../src/ui/transaction/pending_transaction_controller.cpp)
handles the package action buttons.

It is responsible for:

- marking packages for install, upgrade, downgrade, remove, or reinstall
- marking all listed upgrade candidates as pending upgrade actions
- validating self-protected package rules
- clearing pending actions

[src/ui/transaction/pending_transaction_view.cpp](../src/ui/transaction/pending_transaction_view.cpp)
builds the Pending Actions tab and updates package action button labels.

It is responsible for:

- rebuilding the Pending Actions tab
- jumping from a pending action back to its package row
- enabling the Apply button only when actions are pending
- keeping package action button labels aligned with pending actions

[src/ui/transaction/pending_transaction_apply.cpp](../src/ui/transaction/pending_transaction_apply.cpp)
handles preview and apply work.

It is responsible for:

- asking the transaction client for a preview
- showing the review dialog
- starting apply after confirmation
- clearing pending actions after success or after RPM work started
- refreshing package state after apply

Ctrl+Enter starts the same pending-transaction preview as the Apply button.

The pending action data model is this header:
[src/ui/transaction/pending_transaction_state.hpp](../src/ui/transaction/pending_transaction_state.hpp).
Conversion from pending actions to a shared `TransactionRequest` lives in
[src/ui/transaction/pending_transaction_request.cpp](../src/ui/transaction/pending_transaction_request.cpp).

Upgradable rows are visible as repository candidates, but they represent an
installed package with a newer version available. The UI treats the main action
as `Upgrade`. The pending row keeps the visible update NEVRA so the user can
jump back to the selected package, but the transaction request uses a package
name and architecture spec for dnf5daemon. The table keeps the installed version
in the Version column and shows the candidate version in the Update column. The
Repo column shows the repository that provides the update.

In compact Search, List Packages, and List Upgradable views, Remove and
Reinstall act on the currently installed NEVRA represented by the compact
upgrade row. When Latest only is disabled, available rows represent exact
repository versions, so Remove and Reinstall are available only on exact
installed rows. Reinstall is offered only when that exact installed NEVRA is
still available from an enabled repository.

Downgradeable rows are older repository versions of an installed package. The
main action becomes `Downgrade`, and the transaction request uses the selected
exact NEVRA. Install, upgrade, and downgrade actions are unique by package name
and architecture, so marking another install-side target for the same package
replaces the previous one. Exact install actions for install-only packages can
coexist because DNF supports installing several such versions side by side.
Remove and reinstall actions remain tied to exact installed NEVRAs, so parallel
installed versions can be handled independently.
Opening a pending exact installonly action from the Pending Actions tab preserves
that exact install meaning instead of reclassifying the row as an upgrade or
downgrade.

Mark Listed Upgrades marks only valid upgrade candidates from the current table.
It does not mark downgradeable rows or intermediate newer rows.

[src/ui/transaction/pending_transaction_action_rows.cpp](../src/ui/transaction/pending_transaction_action_rows.cpp)
keeps those row-selection rules in one place. It also resolves which package
actions are available for the selected row, so details buttons, context menus,
and row activation use the same answer. This is needed because an update can be
shown from either the installed package list or the upgradable package list. The
helper must not run libdnf queries because it is called while updating GTK
controls.

### Transaction progress

[src/ui/transaction/transaction_progress.cpp](../src/ui/transaction/transaction_progress.cpp) manages the
live progress window shown while apply is running.

[src/ui/transaction/transaction_dialogs.cpp](../src/ui/transaction/transaction_dialogs.cpp)
builds the confirmation dialog shown before apply, the error dialog shown when
preview fails, and the repository signing key prompt. Apply failures remain in
the transaction progress window.

The progress window can receive progress messages after the apply request has
started. The code keeps the progress state alive while queued GTK callbacks are
still pending.

The window also shows daemon-provided progress for the current apply phase.
Downloads use activity mode because the daemon reports individual downloads,
not one reliable total for the full download phase. Verification, transaction
preparation, and RPM processing use the daemon's processed and total values.
The bar is not an overall transaction percentage.

The main window stays open while apply is running so the completion callback can
finish the progress window cleanly.

## Background work pattern

UI code follows this pattern for slow work:

1. Read the current UI state.
2. Create a cancellable task.
3. Run backend work on a worker thread.
4. Return results through `GTask`.
5. On the GTK thread, check whether the result still applies.
6. Update widgets.

This keeps the window responsive and prevents old results from replacing newer
state.

Stop is cooperative. A Stop button cancels the task state immediately, but the
worker still has to reach a safe cancellation point before it can return. Search
and package list workers can now stop while waiting for the shared Base.
Repository refresh cancels both the dnf5daemon refresh call and the later
libdnf5 Base reload. Not every D-Bus or libdnf5 step can stop immediately.

## Refresh rules

Refreshing repositories or applying a transaction can change package metadata.

When that happens, the UI should:

- clear cached package search results
- refresh or republish the installed-package snapshot
- reload the current package view, or clear List Upgradable so stale upgrade
  rows are not left visible
- keep pending action state consistent with the visible rows

The shared task and spinner helpers live in [src/ui/common/widgets.cpp](../src/ui/common/widgets.cpp).
