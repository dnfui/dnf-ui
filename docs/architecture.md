# DNF UI Architecture

This is the overview document for DNF UI.

Use it as the first map when reading the code. The deeper documents are:

- [UI internals](ui.md)
- [Backend internals](backend.md)
- [Transaction service internals](transactions.md)
- [Testing](testing.md)
- [External API assumptions](api-assumptions.md)

## Purpose

DNF UI is a GTK 4 package manager frontend for Fedora.

The main application stays unprivileged. It searches packages, shows package
details, lets the user mark package actions, and shows a review step. Package
apply work is sent to a small D-Bus transaction service, where Polkit can
authorize the privileged step.

## Key Terms

- GTK is the user interface toolkit used to build the window.
- libdnf5 is the Fedora package management library used for package queries and
  transactions.
- Base is the libdnf5 object that holds loaded repository and installed package
  state.
- rpmdb is the local database of packages installed on the system.
- NEVRA means name, epoch, version, release, and architecture. It identifies one
  exact package build.
- EVR means epoch, version, and release. The backend uses it when comparing
  package versions.
- D-Bus is the local message bus used by the GUI and transaction service to
  call each other.
- Polkit is the authorization service used before privileged package apply work.
- GTask is the GLib helper used to run slow work away from the GTK thread and
  return results safely.

## Main Parts

The application is split into five main areas:

- Startup and main window setup
- UI controllers
- libdnf5 backend
- Shared transaction request model
- D-Bus transaction service and GUI client

```mermaid
flowchart TD
    Main[main.cpp] --> App[app.cpp]
    App --> Window[ui/main_window.cpp]
    Window --> Controllers[UI controllers]
    Controllers --> Backend[dnf_backend]
    Controllers --> Client[transaction_service_client.cpp]
    Client --> Service[service/transaction_service.cpp]
    Service --> Backend
```

## Startup

Startup follows a short path:

- [src/main.cpp](../src/main.cpp) calls `app_run_dnfui`
- [src/app.cpp](../src/app.cpp) creates the GTK application and handles activation
- [src/ui/main_window.cpp](../src/ui/main_window.cpp) builds the main window and wires signals

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

## UI Structure

The main window is built once and the controller files own behavior.

- [src/ui/main_window.cpp](../src/ui/main_window.cpp) builds the window, creates shared widget state, and connects signals.
- [src/ui/widgets.hpp](../src/ui/widgets.hpp) groups the widget pointers and shared UI state.
- [src/ui/widgets.cpp](../src/ui/widgets.cpp) owns repository refresh callbacks and task helpers shared by controllers.
- [src/ui/main_menu.cpp](../src/ui/main_menu.cpp) owns top menu actions.
- [src/ui/package_query_controller.cpp](../src/ui/package_query_controller.cpp) owns search, browsing, list cancellation, history, and package-list refresh.
- [src/ui/package_info_controller.cpp](../src/ui/package_info_controller.cpp) owns selection handling and details loading.
- [src/ui/package_table_view.cpp](../src/ui/package_table_view.cpp) owns the package table.
- [src/ui/pending_transaction_controller.cpp](../src/ui/pending_transaction_controller.cpp) owns marking actions, preview, apply, and post-apply refresh.
- [src/ui/transaction_review_dialog.cpp](../src/ui/transaction_review_dialog.cpp) owns the review and error dialogs.
- [src/ui/transaction_progress.cpp](../src/ui/transaction_progress.cpp) owns the live progress window.

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

## Backend Structure

The UI does not use libdnf5 types directly.

The public backend API is [src/dnf_backend/dnf_backend.hpp](../src/dnf_backend/dnf_backend.hpp).
It exposes small value types such as `PackageRow`, `PackageInstallState`, and
`TransactionPreview`.

The backend implementation is split by responsibility:

- [src/base_manager.cpp](../src/base_manager.cpp) owns the shared libdnf5 `Base`.
- [src/dnf_backend/dnf_query.cpp](../src/dnf_backend/dnf_query.cpp) builds package rows for search, browse, and installed-list views.
- [src/dnf_backend/dnf_details.cpp](../src/dnf_backend/dnf_details.cpp) formats package details, files, dependencies, and changelog text.
- [src/dnf_backend/dnf_state.cpp](../src/dnf_backend/dnf_state.cpp) owns installed-package snapshot state and package status classification.
- [src/dnf_backend/dnf_transaction.cpp](../src/dnf_backend/dnf_transaction.cpp) resolves previews and applies transactions.

Most query and details calls take serialized read access to the shared Base.
That access is exclusive inside `BaseManager` because read-only `PackageQuery`
work can still touch shared libdnf5 `Base` internals. Transaction preview and
apply take write access because libdnf5 transaction work changes Base state
while it is being resolved or run.

The shared Base does not request changelog `other` metadata. Changelog details
read installed packages from the shared Base first because rpmdb changelog
metadata is local. Available-package changelogs use a short-lived temporary Base
so that normal list, search, and transaction paths do not keep that optional
metadata resident.

## Package List Model

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

## Transaction Boundary

Search, browsing, and details stay inside the GUI process.

Preview and apply go through the transaction service:

- GUI client: [src/transaction_service_client.cpp](../src/transaction_service_client.cpp)
- service runtime and shutdown: [src/service/transaction_service.cpp](../src/service/transaction_service.cpp)
- service request objects: [src/service/transaction_service_request_objects.cpp](../src/service/transaction_service_request_objects.cpp)
- service preview and apply workers: [src/service/transaction_service_workers.cpp](../src/service/transaction_service_workers.cpp)
- service authorization: [src/service/transaction_service_authorization.cpp](../src/service/transaction_service_authorization.cpp)
- service signals: [src/service/transaction_service_signals.cpp](../src/service/transaction_service_signals.cpp)
- shared D-Bus names: [src/service/transaction_service_dbus.hpp](../src/service/transaction_service_dbus.hpp)
- shared request model: [src/transaction_request.hpp](../src/transaction_request.hpp)

```mermaid
flowchart TD
    Pending[Pending transaction controller] --> Request[TransactionRequest]
    Request --> Client[GUI D-Bus client]
    Client --> Service[Transaction service]
    Service --> Preview[Resolve preview]
    Preview --> Confirm[GUI confirmation dialog]
    Confirm --> Apply[Apply request]
    Apply --> Auth[Polkit authorization]
    Auth --> Run[Run transaction]
    Run --> Refresh[GUI refreshes package state]
```

The service creates one D-Bus request object for each transaction request. The
GUI reads preview and final result state from that object and releases it when it
is no longer needed.

## Packaging

Service install files live under [packaging](../packaging).

The important files are:

- [packaging/com.fedora.Dnfui.Transaction1.service](../packaging/com.fedora.Dnfui.Transaction1.service)
- [packaging/com.fedora.Dnfui.Transaction1.conf](../packaging/com.fedora.Dnfui.Transaction1.conf)
- [packaging/com.fedora.dnfui.policy](../packaging/com.fedora.dnfui.policy)
- [packaging/dnfui-service.service](../packaging/dnfui-service.service)

Meson owns the real build and install rules. The `Makefile` is a task runner for
common developer commands.

## Reading Order

A practical reading order for new contributors:

1. [src/main.cpp](../src/main.cpp)
2. [src/app.cpp](../src/app.cpp)
3. [src/ui/main_window.cpp](../src/ui/main_window.cpp)
4. [src/ui/widgets.hpp](../src/ui/widgets.hpp)
5. [src/ui/package_query_controller.cpp](../src/ui/package_query_controller.cpp)
6. [src/ui/pending_transaction_controller.cpp](../src/ui/pending_transaction_controller.cpp)
7. [src/dnf_backend/dnf_backend.hpp](../src/dnf_backend/dnf_backend.hpp)
8. [src/base_manager.cpp](../src/base_manager.cpp)
9. [src/dnf_backend/dnf_query.cpp](../src/dnf_backend/dnf_query.cpp)
10. [src/transaction_service_client.cpp](../src/transaction_service_client.cpp)
11. [docs/transactions.md](transactions.md)
12. [src/service/transaction_service.cpp](../src/service/transaction_service.cpp)
13. [src/service/transaction_service_request_objects.cpp](../src/service/transaction_service_request_objects.cpp)
14. [src/service/transaction_service_workers.cpp](../src/service/transaction_service_workers.cpp)
