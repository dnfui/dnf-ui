# UI Internals

This document explains how the GTK user interface is organized.

For source-backed GTK and GIO assumptions, see
[External API assumptions](api-assumptions.md).

## Main Idea

`main_window.cpp` builds the widgets. Controller files own behavior.

The shared widget state lives in [src/ui/widgets.hpp](../src/ui/widgets.hpp).
Controller files receive a `SearchWidgets` pointer and use it to update the
parts of the window they own.

This keeps widget construction, package query behavior, package details, and
pending transaction behavior in separate files.

## Window Construction

[src/ui/main_window.cpp](../src/ui/main_window.cpp) creates the main window.

It is responsible for:

- creating the top menu
- creating search and list buttons
- creating the package table
- creating the package details notebook
- creating the pending actions tab
- creating transaction action buttons
- connecting GTK signals to controller callbacks
- saving window size and pane positions

It should not contain package query logic or transaction apply logic. Those
belong in the controller files.

## Shared Widget State

[src/ui/widgets.hpp](../src/ui/widgets.hpp) groups the widget pointers into
smaller structs:

- `PackageQueryWidgets` for search controls and status
- `PackageResultWidgets` for the package table and details notebook
- `PendingTransactionWidgets` for marked actions and apply controls
- `WindowStateWidgets` for window-level state
- `SearchWidgets` as the top-level shared state passed to controllers

This state is not meant to hide ownership. It is a practical place to store
GTK pointers that several controllers need.

## Controller Files

### Package Query Controller

[src/ui/package_query_controller.cpp](../src/ui/package_query_controller.cpp)
owns the package list workflows:

- list installed packages
- browse available and installed packages together
- list installed packages that have available updates
- search packages
- stop the active list request
- restore a search from history
- clear the package list
- reload the current view after package state changes

Long-running package queries run on worker threads through `GTask`. Completion
callbacks run on the GTK thread before they update widgets.

Search results are cached in [src/ui/package_query_cache.cpp](../src/ui/package_query_cache.cpp).
The cache is tied to the current backend Base generation, so a repository
refresh or transaction rebuild cannot reuse outdated package rows.

### Package Info Controller

[src/ui/package_info_controller.cpp](../src/ui/package_info_controller.cpp)
owns the details pane for the selected package.

It updates:

- package details text
- installed file list
- dependencies
- changelog
- install, remove, and reinstall button sensitivity

Details are loaded in the background. The controller records the selected NEVRA
and backend generation when the task starts. If the selected package changes or
the backend generation changes, the old result is ignored.

### Package Table View

[src/ui/package_table_view.cpp](../src/ui/package_table_view.cpp) owns the
package table model and columns.

It wraps each `PackageRow` in a GTK object so the column view can sort and
select rows. It also refreshes status badges when pending actions change.

[src/ui/package_table_status.cpp](../src/ui/package_table_status.cpp) keeps the
status text, tooltip text, and CSS classes separate from table construction.

[src/ui/package_table_context_menu.cpp](../src/ui/package_table_context_menu.cpp)
owns right-click actions for package rows.

### Pending Transaction Controller

[src/ui/pending_transaction_controller.cpp](../src/ui/pending_transaction_controller.cpp)
owns package actions before they are applied.

It is responsible for:

- marking packages for install, remove, or reinstall
- rebuilding the Pending Actions tab
- validating self-protected package rules
- asking the transaction service for a preview
- showing the review dialog
- starting apply after confirmation
- clearing pending actions after a successful apply
- refreshing package state after apply

The pending action data model lives in [src/ui/pending_transaction_state.hpp](../src/ui/pending_transaction_state.hpp).
Conversion from pending actions to a shared `TransactionRequest` lives in
[src/ui/pending_transaction_request.cpp](../src/ui/pending_transaction_request.cpp).

### Transaction Progress

[src/ui/transaction_progress.cpp](../src/ui/transaction_progress.cpp) owns the
transaction review dialog and progress window.

The progress window can receive progress messages after the apply request has
started. The code keeps the progress state alive while queued GTK callbacks are
still pending.

## Background Work Pattern

UI code follows this pattern for slow work:

1. Read the current UI state.
2. Create a cancellable task.
3. Run backend work on a worker thread.
4. Return results through `GTask`.
5. On the GTK thread, check whether the result still applies.
6. Update widgets.

This keeps the window responsive and prevents old results from replacing newer
state.

## Refresh Rules

Refreshing repositories or applying a transaction can change package metadata.

When that happens, the UI should:

- clear cached package search results
- refresh or republish the installed-package snapshot
- reload the current package view
- keep pending action state consistent with the visible rows

The shared refresh helpers live in [src/ui/widgets.cpp](../src/ui/widgets.cpp).
