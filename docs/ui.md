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
- `PendingTransactionWidgets` for marked actions and apply controls
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
- [src/ui/package_query/package_query_controller_internal.hpp](../src/ui/package_query/package_query_controller_internal.hpp)
  declares the shared functions used by those files.

Long-running package queries run on worker threads through `GTask`. Completion
callbacks run on the GTK thread before they update widgets.

The bottom bar shows the visible row count on the left and the last completed
package query time on the right.

Search results are cached in [src/ui/package_query/package_query_cache.cpp](../src/ui/package_query/package_query_cache.cpp).
The cache is tied to the current backend Base generation and a cache epoch kept
by the query cache layer. Repository refreshes, transaction follow-up refreshes,
and installed-state refreshes clear cached search rows and advance that epoch,
so older search workers cannot repopulate the cache with rows the UI has already
invalidated. Dropping the cached Base to save memory does not invalidate search
rows by itself.

[src/ui/refresh/repository_refresh_controller.cpp](../src/ui/refresh/repository_refresh_controller.cpp)
owns the Refresh Repositories button workflow. It refreshes dnf5daemon metadata,
rebuilds the libdnf5 Base, updates the lower-right progress text, and clears
stale upgradable rows after repository metadata changes.

### Package details controller

[src/ui/details/package_details_controller.cpp](../src/ui/details/package_details_controller.cpp)
updates the details pane for the selected package.

It updates:

- package details text
- installed file list
- dependencies
- changelog, loaded only when the Changelog tab is opened
- install, remove, and reinstall button sensitivity

Details are loaded in the background. The controller records the selected NEVRA
and backend generation when each task starts. If the selected package changes or
the backend generation changes, the old result is ignored. Changelog loading is
kept separate because available-package changelog data can require extra
repository metadata.

### Package table view

[src/ui/package_table/package_table_view.cpp](../src/ui/package_table/package_table_view.cpp) builds the
package table, including column setup, selection, and status refresh.

The table columns can be shown or hidden from `View -> Columns`, and the same
menu can restore the default column set. The setting is stored in `dnfui.conf`
as `package_table_hidden_columns`, using stable column ids so new default-visible
columns can be added without hiding them for existing users. Older
`package_table_columns` settings are migrated when they are read.

`File -> Export Package List...` or Ctrl+E writes the currently visible package
table rows to a CSV file. It exports the table model that is already shown to
the user instead of running another backend query.

[src/ui/package_table/package_table_columns.cpp](../src/ui/package_table/package_table_columns.cpp) owns the
package table column definitions, stable column ids, saved visibility settings,
and config migration.

[src/ui/package_table/package_table_model.cpp](../src/ui/package_table/package_table_model.cpp) contains the
GTK object wrapper used to store package rows in the table model.

[src/ui/package_table/package_table_sort.cpp](../src/ui/package_table/package_table_sort.cpp) contains package
table cell text and sorting rules.

[src/ui/package_table/package_table_export.cpp](../src/ui/package_table/package_table_export.cpp) exports the
current table rows to CSV.
[src/ui/package_table/package_table_export_csv.cpp](../src/ui/package_table/package_table_export_csv.cpp) formats the
CSV text and is tested without opening a GTK file dialog.

[src/ui/package_table/package_table_status.cpp](../src/ui/package_table/package_table_status.cpp) keeps the
status text, tooltip text, and CSS classes separate from table construction.

[src/ui/package_table/package_table_context_menu.cpp](../src/ui/package_table/package_table_context_menu.cpp)
builds right-click actions for package rows.

### Transaction history

`Package -> Transaction History...` opens a read-only window backed by libdnf5
transaction history. It lists recent package changes and lets the user filter
them by package, action, result, date range, repository, architecture, or
command text.

The history window lives in [src/ui/history/transaction_history_view.cpp](../src/ui/history/transaction_history_view.cpp).
It loads history on a worker thread and displays value objects from the backend
instead of libdnf5 objects. The feature is intentionally read-only. It does not
offer rollback, replay, or undo actions.

### Pending transaction controller

[src/ui/transaction/pending_transaction_controller.cpp](../src/ui/transaction/pending_transaction_controller.cpp)
handles the package action buttons.

It is responsible for:

- marking packages for install, upgrade, remove, or reinstall
- validating self-protected package rules
- clearing pending actions

[src/ui/transaction/pending_transaction_view.cpp](../src/ui/transaction/pending_transaction_view.cpp)
builds the Pending Actions tab.

It is responsible for:

- rebuilding the Pending Actions tab
- jumping from a pending action back to its package row
- enabling the Apply button only when actions are pending

[src/ui/transaction/pending_transaction_apply.cpp](../src/ui/transaction/pending_transaction_apply.cpp)
handles preview and apply work.

It is responsible for:

- asking the transaction client for a preview
- showing the review dialog
- starting apply after confirmation
- clearing pending actions after a successful apply
- refreshing package state after apply

The pending action data model lives in [src/ui/transaction/pending_transaction_state.hpp](../src/ui/transaction/pending_transaction_state.hpp).
Conversion from pending actions to a shared `TransactionRequest` lives in
[src/ui/transaction/pending_transaction_request.cpp](../src/ui/transaction/pending_transaction_request.cpp).

Upgradable rows are visible as repository candidates, but they represent an
installed package with a newer version available. The UI treats the main action
as `Upgrade`. The pending row keeps the visible update NEVRA so the user can
jump back to the selected package, but the transaction request uses a package
name and architecture spec for dnf5daemon. The table keeps the installed version
in the Version column and shows the candidate version in the Update column. The
Repo column shows the repository that provides the update. Remove and reinstall
act on the currently installed NEVRA for the same package name and architecture.

[src/ui/transaction/pending_transaction_action_rows.cpp](../src/ui/transaction/pending_transaction_action_rows.cpp) keeps those
row-selection rules in one place. This is needed because an update can be shown
from either the installed package list or the upgradable package list. The helper
must not run libdnf queries because it is called while updating GTK controls.

### Transaction progress

[src/ui/transaction/transaction_progress.cpp](../src/ui/transaction/transaction_progress.cpp) manages the
live progress window shown while apply is running.

[src/ui/transaction/transaction_dialogs.cpp](../src/ui/transaction/transaction_dialogs.cpp)
builds the confirmation dialog shown before apply, the error dialog shown when
preview or apply fails, and the repository signing key prompt.

The progress window can receive progress messages after the apply request has
started. The code keeps the progress state alive while queued GTK callbacks are
still pending.

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
