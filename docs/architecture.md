# DNF UI architecture

This is the overview document for DNF UI.

Use it as the first map when reading the code. The deeper documents are:

- [UI internals](ui.md)
- [Backend internals](backend.md)
- [Transaction flow](transactions.md)
- [Testing](testing.md)
- [External API assumptions](api-assumptions.md)
- [Project rules](project-rules.md)

## Purpose

DNF UI is a GTK 4 package manager frontend for Fedora.

The main application stays unprivileged. It searches packages, shows package
details, lets the user mark package actions, and shows a review step. Package
changes are sent to DNF5 dnf5daemon, which owns the privileged package work
and Polkit behavior.

## Key terms

- GTK is the user interface toolkit used to build the window.
- libdnf5 is the Fedora package management library used for package queries and transactions.
- Base is the libdnf5 object that holds loaded repository and installed package
  state.
- rpmdb is the local database of packages installed on the system.
- NEVRA means name, epoch, version, release, and architecture. It identifies one
  exact package build.
- EVR means epoch, version, and release. The backend uses it when comparing
  package versions.
- D-Bus is the local message bus used by the GUI to call dnf5daemon.
- Polkit is the authorization service used by dnf5daemon before privileged package apply work.
- GTask is the GLib helper used to run slow work away from the GTK thread and return results safely.

## Main parts

The application is split into five main areas:

- Startup and main window setup
- UI controllers
- libdnf5 backend
- Shared transaction request model
- dnf5daemon transaction client

```mermaid
flowchart TD
    Main[main.cpp] --> App[app.cpp]
    App --> Window[ui/main_window.cpp]
    Window --> Layout[ui/main_window_layout.cpp]
    Window --> Controllers[UI controllers]
    Controllers --> Backend[dnf_backend]
    Controllers --> Client[transaction_service_client.cpp]
    Client --> Daemon[DNF5 dnf5daemon]
```

## Startup

Startup follows a short path:

- [src/main.cpp](../src/main.cpp) calls `app_run_dnfui`
- [src/app.cpp](../src/app.cpp) creates the GTK application and handles activation
- [src/ui/main_window.cpp](../src/ui/main_window.cpp) creates the main window and wires signals
- [src/ui/main_window_layout.cpp](../src/ui/main_window_layout.cpp) builds the main window widget tree

After the window is created, `app.cpp` also starts two background tasks:

- backend warm up, so the first package query is faster
- periodic installed-package snapshot refresh

```mermaid
flowchart TD
    Main[main.cpp] --> Run[app_run_dnfui]
    Run --> Activate[GTK activate]
    Activate --> Window[main_window_create]
    Activate --> Warmup[backend warm up]
    Activate --> Refresh[periodic installed refresh]
```

## UI structure

The main window is built once and the controller files own behavior.

- [src/ui/main_window.cpp](../src/ui/main_window.cpp) creates shared widget state and connects signals.
- [src/ui/main_window_layout.cpp](../src/ui/main_window_layout.cpp) builds the main window widget tree.
- [src/ui/widgets.hpp](../src/ui/widgets.hpp) groups the widget pointers and shared UI state.
- [src/ui/widgets.cpp](../src/ui/widgets.cpp) handles repository refresh callbacks and task helpers shared by controllers.
- [src/ui/main_menu.cpp](../src/ui/main_menu.cpp) handles top menu actions.
- [src/ui/package_query_controller.cpp](../src/ui/package_query_controller.cpp) handles the public search, list, history, clear, and reload callbacks.
- [src/ui/package_query_controls.cpp](../src/ui/package_query_controls.cpp) handles active package-query request state, Stop button handling, cancellation, and refresh completion.
- [src/ui/package_query_tasks.cpp](../src/ui/package_query_tasks.cpp) contains package-query worker tasks and completion handlers.
- [src/ui/package_info_controller.cpp](../src/ui/package_info_controller.cpp) handles selection and details loading.
- [src/ui/package_table_view.cpp](../src/ui/package_table_view.cpp) builds the package table.
- [src/ui/package_table_model.cpp](../src/ui/package_table_model.cpp) stores package rows in GTK objects.
- [src/ui/package_table_sort.cpp](../src/ui/package_table_sort.cpp) contains package table sorting rules.
- [src/ui/pending_transaction_controller.cpp](../src/ui/pending_transaction_controller.cpp) handles package action buttons.
- [src/ui/pending_transaction_view.cpp](../src/ui/pending_transaction_view.cpp) builds the Pending Actions tab.
- [src/ui/pending_transaction_apply.cpp](../src/ui/pending_transaction_apply.cpp) handles preview, apply, and post-apply refresh.
- [src/ui/transaction_review_dialog.cpp](../src/ui/transaction_review_dialog.cpp) builds the review and error dialogs.
- [src/ui/transaction_progress.cpp](../src/ui/transaction_progress.cpp) manages the live progress window.

The UI controller pattern follows this shape:

```mermaid
flowchart TD
    User[User action] --> Signal[GTK signal]
    Signal --> Controller[Controller callback]
    Controller --> State[Update shared UI state]
    Controller --> Work[Start backend or service work]
    Work --> Finish[Completion callback on GTK thread]
    Finish --> UI[Refresh visible widgets]
```

## Backend structure

The UI does not use libdnf5 types directly.

The public backend API is [src/dnf_backend/dnf_backend.hpp](../src/dnf_backend/dnf_backend.hpp).
It exposes small value types such as `PackageRow`, `PackageInstallState`, and
`TransactionPreview`.

The backend implementation is split by responsibility:

- [src/base_manager.cpp](../src/base_manager.cpp) manages the shared libdnf5 `Base`.
- [src/dnf_backend/dnf_query.cpp](../src/dnf_backend/dnf_query.cpp) builds package rows for search, browse, and installed-list views.
- [src/dnf_backend/dnf_details.cpp](../src/dnf_backend/dnf_details.cpp) formats package details, files, dependencies, and changelog text.
- [src/dnf_backend/dnf_state.cpp](../src/dnf_backend/dnf_state.cpp) keeps installed-package snapshot state and package status classification.
- [src/dnf_backend/dnf_transaction.cpp](../src/dnf_backend/dnf_transaction.cpp) resolves previews and applies transactions.
- [src/dnf_backend/dnf_transaction_callbacks.cpp](../src/dnf_backend/dnf_transaction_callbacks.cpp) adapts libdnf download and rpm callbacks into progress lines.
- [src/dnf_backend/dnf_transaction_format.cpp](../src/dnf_backend/dnf_transaction_format.cpp) keeps shared transaction text formatting out of the resolver.

Most query and details calls take serialized read access to the shared Base.
That access is exclusive inside `BaseManager` because read-only `PackageQuery`
work can still touch shared libdnf5 `Base` internals. The remaining local
backend transaction helpers take write access because libdnf5 transaction work
changes Base state while it is being resolved or run. The normal GUI preview
and apply path goes through dnf5daemon instead.

The shared Base does not request changelog `other` metadata. Changelog details
read installed packages from the shared Base because rpmdb changelog metadata
is local. Available update rows use the currently installed package with the
same name and architecture instead of loading repository changelog metadata.

## Package list model

The main list shows one row for each package name and architecture pair.

When repository metadata is available, repository candidates are shown. Installed
packages that do not have a visible repository candidate are added as local-only
rows. Installed packages can also be shown as upgradeable or newer than the
repository candidate.

The installed snapshot in [src/dnf_backend/dnf_state.cpp](../src/dnf_backend/dnf_state.cpp)
is important because it lets the UI answer:

- whether an exact NEVRA is installed
- whether a row is available, installed, local-only, or upgradeable
- whether a package owns the running GUI executable and must be protected from removal inside the app

## Transaction boundary

Search, browsing, and details stay inside the GUI process.

Preview and apply go through DNF5 dnf5daemon:

- GUI client: [src/transaction_service_client.cpp](../src/transaction_service_client.cpp)
- GUI client D-Bus calls: [src/transaction_service_client_dbus.cpp](../src/transaction_service_client_dbus.cpp)
- GUI client wait handling: [src/transaction_service_client_wait.cpp](../src/transaction_service_client_wait.cpp)
- shared request model: [src/transaction_request.hpp](../src/transaction_request.hpp)

```mermaid
flowchart TD
    Pending[Pending transaction controller] --> Request[TransactionRequest]
    Request --> Client[Transaction client]
    Client --> Daemon[DNF5 dnf5daemon]
    Daemon --> Preview[Resolve preview]
    Preview --> Confirm[GUI confirmation dialog]
    Confirm --> Apply[Apply through dnf5daemon]
    Apply --> Auth[dnf5daemon Polkit behavior]
    Auth --> Run[Run transaction]
    Run --> Refresh[GUI refreshes package state]
```

The client opens one dnf5daemon session for each prepared transaction. The GUI
shows the resolved preview, applies through the same session if the user
confirms, and closes the session when it is no longer needed.

## Packaging

Packaging metadata lives under [packaging](../packaging).

DNF UI requires Fedora `dnf5daemon-server` for package changes. It does not
install its own transaction service, Polkit policy, D-Bus policy, or systemd
unit for package apply work.

The security boundary is described in [docs/systemd-hardening.md](systemd-hardening.md).

Meson owns the real build and install rules. The `Makefile` is a task runner for
common developer commands.

## Reading order

A practical reading order for new contributors:

1. [src/main.cpp](../src/main.cpp)
2. [src/app.cpp](../src/app.cpp)
3. [src/ui/main_window.cpp](../src/ui/main_window.cpp)
4. [src/ui/main_window_layout.cpp](../src/ui/main_window_layout.cpp)
5. [src/ui/widgets.hpp](../src/ui/widgets.hpp)
6. [src/ui/package_query_controller.cpp](../src/ui/package_query_controller.cpp)
7. [src/ui/pending_transaction_controller.cpp](../src/ui/pending_transaction_controller.cpp)
8. [src/ui/pending_transaction_view.cpp](../src/ui/pending_transaction_view.cpp)
9. [src/ui/pending_transaction_apply.cpp](../src/ui/pending_transaction_apply.cpp)
10. [src/dnf_backend/dnf_backend.hpp](../src/dnf_backend/dnf_backend.hpp)
11. [src/base_manager.cpp](../src/base_manager.cpp)
12. [src/dnf_backend/dnf_query.cpp](../src/dnf_backend/dnf_query.cpp)
13. [src/transaction_service_client.cpp](../src/transaction_service_client.cpp)
13. [src/transaction_service_client_dbus.cpp](../src/transaction_service_client_dbus.cpp)
14. [src/transaction_service_client_wait.cpp](../src/transaction_service_client_wait.cpp)
15. [docs/transactions.md](transactions.md)
