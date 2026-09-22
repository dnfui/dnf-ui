Name:           dnf-ui
Version:        0.6.4
Release:        1%{?dist}
Summary:        GTK interface for DNF5

License:        MIT
URL:            https://github.com/dnfui/dnf-ui
Source0:        %{url}/archive/refs/tags/%{version}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  appstream
BuildRequires:  dbus-daemon
BuildRequires:  desktop-file-utils
BuildRequires:  gcc-c++
BuildRequires:  gettext
BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(catch2-with-main)
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(libdnf5)

Requires:       dnf5daemon-server

%description
DNF UI is a graphical interface for DNF5 package management built with GTK 4
and libdnf5. It supports browsing available and installed packages, reviewing
transaction changes, and applying package transactions through dnf5daemon.

%prep
%autosetup -p1

%build
%meson -Dbuild_tests=true -Dwarning_level=3 -Dfinal_build=true
%meson_build

%install
%meson_install
%find_lang %{name}

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.dnfui.dnfui.desktop
appstreamcli validate --no-net %{buildroot}%{_datadir}/metainfo/io.github.dnfui.dnfui.metainfo.xml
%meson_test

%files -f %{name}.lang
%license LICENSE
%doc README.md
%{_bindir}/dnfui
%{_datadir}/applications/io.github.dnfui.dnfui.desktop
%{_datadir}/icons/hicolor/48x48/apps/io.github.dnfui.dnfui.png
%{_datadir}/icons/hicolor/64x64/apps/io.github.dnfui.dnfui.png
%{_datadir}/icons/hicolor/128x128/apps/io.github.dnfui.dnfui.png
%{_datadir}/icons/hicolor/256x256/apps/io.github.dnfui.dnfui.png
%{_datadir}/icons/hicolor/512x512/apps/io.github.dnfui.dnfui.png
%{_datadir}/icons/hicolor/1024x1024/apps/io.github.dnfui.dnfui.png
%{_datadir}/metainfo/io.github.dnfui.dnfui.metainfo.xml

%changelog
* Tue Sep 22 2026 ErikMN <dnfui@proton.me> - 0.6.4-1
- Show skipped packages as warnings instead of failing transaction previews
- Allow valid package changes to proceed when other packages are skipped
- Avoid reporting skipped upgrades as already up to date
- Show Release and Update Release columns by default
- Keep package browsing working after libdnf5 is upgraded while DNF UI is running
- Keep the main window disabled while package transactions are being applied
- Improve shutdown responsiveness while startup package data is loading

* Mon Sep 07 2026 ErikMN <dnfui@proton.me> - 0.6.3-1
- Show phase progress while applying package transactions
- Keep the available upgrade count current after package, repository, and transaction changes
- Speed up transaction preview preparation for installs and upgrades
- Close transaction dialogs automatically when the main window is closed

* Tue Aug 25 2026 ErikMN <dnfui@proton.me> - 0.6.2-1
- Show the available package upgrade count after startup and repository refresh

* Sat Aug 22 2026 ErikMN <dnfui@proton.me> - 0.6.1-1
- Allow sorting repositories by enabled state, ID, or name
- Highlight staged repository enable and disable changes
- Keep package and pending-action state accurate when a transaction reports failure after RPM work has started

* Sat Aug 22 2026 ErikMN <dnfui@proton.me> - 0.6.0-1
- Add a repository manager for viewing, enabling, and disabling configured repositories
- Review staged repository changes before applying them through dnf5daemon
- Add a Clear Pending action for discarding staged repository changes
- Make repository manager columns resizable for long repository IDs and names
- Refresh package data after repository changes and report the reload result

* Tue Aug 18 2026 ErikMN <dnfui@proton.me> - 0.5.6-1
- Add keyboard shortcuts for refreshing repositories and applying pending changes
- Improve toolbar grouping and package action icons
- Improve icon compatibility across GTK icon themes

* Wed Aug 05 2026 ErikMN <dnfui@proton.me> - 0.5.5-1
- Allow remove-only transactions in cold offline mode
- Show clearer repository and package download errors
- Keep apply failures in the transaction progress window without opening a second popup
- Improve cancellation responsiveness during transaction preparation and repository operations

* Sun Aug 02 2026 ErikMN <dnfui@proton.me> - 0.5.4-1
- Restore reinstall actions for installed packages that still exist in enabled repositories

* Sun Aug 02 2026 ErikMN <dnfui@proton.me> - 0.5.3-1
- Fix package-state handling for parallel installed versions and installonly packages
- Keep self-protection active after failed rediscovery
- Validate protected replacements from the complete dnf5daemon preview
- Require exact repository availability before offering reinstall
- Harden CSV export status updates and spreadsheet formula handling

* Sat Aug 01 2026 ErikMN <dnfui@proton.me> - 0.5.2-1
- Fix installed-row update information and upgrade actions in all-version views
- Keep installed-state refresh synchronized with the shared package Base
- Translate remaining package detail and transaction validation messages
- Show upgrade-target changelogs consistently and sort changelog entries newest first
- Avoid repeated config writes while dragging the package details divider

* Fri Jul 31 2026 ErikMN <dnfui@proton.me> - 0.5.1-1
- Fix package details and table values in all-version views
- Keep remove and reinstall actions on exact installed package rows
- Keep upgrade actions and pending status on the correct row

* Thu Jul 30 2026 ErikMN <dnfui@proton.me> - 0.5.0-1
- Add optional all-version browsing for Search and List Packages
- Add downgrade support for selected older package versions
- Keep Latest only as the default compact package view
- Update package details, status text, documentation, translations, and tests for multi-version browsing

* Sun Jul 26 2026 ErikMN <dnfui@proton.me> - 0.4.3-1
- Align transaction documentation with prepared dnf5daemon sessions
- Make repository key trust prompts cancellable
- Stabilize package table display and sorting values across installed-state refreshes
- Resolve selected installed package state from one snapshot
- Sort package versions and releases with RPM version rules
- Keep Details Status live and show it only when the Status column is hidden

* Sat Jul 25 2026 ErikMN <dnfui@proton.me> - 0.4.2-1
- Clarify installed-package snapshot change reporting
- Use typed daemon upgrade publication results instead of status-message control flow
- Simplify List Upgradable refresh ownership
- Fix UTF-8 case-insensitive filtering in transaction history
- Use one pending-action resolver for package table Status text, rendering, and export
- Show a retry message when stale package query results are rejected

* Tue Jul 21 2026 ErikMN <dnfui@proton.me> - 0.4.1-1
- Improve package search and backend state handling
- Simplify asynchronous ownership and BaseManager locking
- Show installed package origin in package details
- Update backend documentation

* Sun Jul 19 2026 ErikMN <dnfui@proton.me> - 0.4.0-1
- Use dnf5daemon as the source for List Upgradable
- Keep daemon-reported upgrades visible when optional package metadata is unavailable
- Reject stale upgrade rows after package or repository state changes
- Keep selected upgrades tied to daemon preview and apply
- Reduce package-table memory use during repeated large-list rebuilds
- Remove the obsolete transaction item-count limit

* Thu Jul 16 2026 ErikMN <dnfui@proton.me> - 0.3.8-1
- Add bulk marking of listed upgrades for selective system updates
- Keep bulk and individual upgrade actions consistent after metadata changes
- Prevent marking outdated package rows while a query is running
- Make package double-click follow the normal pending action rules

* Wed Jul 15 2026 ErikMN <dnfui@proton.me> - 0.3.7-1
- Allow DNF UI to mark its own package for upgrade
- Show dnf5daemon warnings when a preview returns no package changes
- Improve Fedora package review helper and source RPM packaging checks
- Update documentation for current transaction, backend, and test behavior

* Sun Jul 12 2026 ErikMN <dnfui@proton.me> - 0.3.6-1
- Load package files and dependencies only when their tabs are opened
- Keep the transaction history browser usable around transaction previews
- Show dnf5daemon resolve warnings in transaction previews
- Add issue reporting link to the About dialog

* Thu Jul 09 2026 ErikMN <dnfui@proton.me> - 0.3.5-1
- Change the desktop application ID to io.github.dnfui.dnfui
- Update desktop, AppStream, icon, RPM, and CI packaging paths for the new ID
- Serialize temporary changelog Base lifetime through BaseManager

* Wed Jul 08 2026 ErikMN <dnfui@proton.me> - 0.3.4-1
- Keep transaction history scans from blocking normal package searches
- Fix closing the transaction history browser while history is still loading
- Keep the final transaction progress status visible after late progress updates

* Tue Jul 07 2026 ErikMN <dnfui@proton.me> - 0.3.3-1
- Add read-only transaction history browser
- Add transaction history filtering, paging, and keyboard shortcuts
- Improve responsiveness when jumping to pending package actions
- Improve translations for package details and status text
- Polish main window styling

* Thu Jul 02 2026 ErikMN <dnfui@proton.me> - 0.3.2-1
- Load package changelogs only when the Changelog tab is opened
- Add package table CSV export from the File menu and Ctrl+E
- Improve the empty package table message with useful shortcuts
- Translate package details labels

* Tue Jun 30 2026 ErikMN <dnfui@proton.me> - 0.3.1-1
- Show package status in the Info tab
- Fix search casing so installed packages keep the same repository status
- Improve dnf5daemon apply cancellation during UI teardown
- Move backend BaseManager code into the backend source directory
- Clean up internal UI names and contributor documentation links

* Sat Jun 27 2026 ErikMN <dnfui@proton.me> - 0.3.0-1
- Add configurable package table columns with saved visibility settings
- Add reset action for package table columns
- Sync manual repository refresh with dnf5daemon and the UI package cache
- Align List Upgradable with the resolved dnf5daemon Upgrade All preview
- Avoid stale upgrade rows and details after repository refresh

* Sat Jun 20 2026 ErikMN <dnfui@proton.me> - 0.2.5-1
- Speed up List Upgradable and Upgrade All preview preparation
- Add repository signing key approval prompts for dnf5daemon transactions
- Improve preview cancellation and selected package reload responsiveness
- Keep transaction summary dialog callbacks safe during window shutdown
- Add Ctrl+L shortcut for clearing the package list

* Fri Jun 19 2026 ErikMN <dnfui@proton.me> - 0.2.4-1
- Align List Upgradable with dnf5daemon transaction previews
- Add wildcard package search using * and ?
- Improve repository refresh and transaction apply coordination
- Make List Upgradable Stop cancel daemon preview work
- Clarify local repository-candidate status wording

* Fri Jun 19 2026 ErikMN <dnfui@proton.me> - 0.2.3-1
- Show installed and update versions separately for upgradable packages
- Show the repository that provides each upgradable package
- Allow DNF UI and dnf5daemon upgrades while still blocking unsafe removal
- Keep package query Stop feedback visible until backend work finishes
- Improve transaction preview safety and replaced-package reporting

* Tue Jun 16 2026 ErikMN <dnfui@proton.me> - 0.2.2-1
- Add repository signing key approval during transactions
- Show live repository refresh progress
- Improve cancellation handling for search and selected-package reloads
- Harden dnf5daemon preview parsing and resolve result handling
- Keep exact package reloads off the GTK thread

* Sat Jun 13 2026 ErikMN <dnfui@proton.me> - 0.2.1-1
- Force repository metadata refresh when using the Refresh repositories button
- Allow repository refresh to be stopped from the UI while the current repo check finishes
- Improve transaction safety around dnf5daemon session handling
- Use dnf5daemon's native Upgrade All handling
- Block transactions that would remove dnf5daemon-server
- Prevent Upgrade All from upgrading DNF UI itself
- Avoid periodic installed-package refresh while a transaction is applying
- Fix failed preview session cleanup

* Mon Jun 08 2026 ErikMN <dnfui@proton.me> - 0.2.0-1
- Use DNF5 dnf5daemon for transaction preview and apply
- Remove the custom privileged transaction service
- Add dnf5daemon transaction tests and native apply test target
- Improve stale-preview apply failures
- Block removal of dnf5daemon-server from DNF UI

* Tue Jun 02 2026 ErikMN <dnfui@proton.me> - 0.1.7-1
- Cache improvements
- Search improvements
- Search timer
- UI polish

* Wed May 27 2026 ErikMN <dnfui@proton.me> - 0.1.6-1
- Make transaction preview fail closed for unsupported resolved actions
- Split test-only transaction service hooks out of the installed service
- Authorize system-bus preview requests before privileged preview work starts
- Fix search cache reuse after the shared backend Base is dropped and recreated
- Improve transaction service test coverage and supporting documentation

* Fri May 15 2026 ErikMN <dnfui@proton.me> - 0.1.5-1
- Polish the package list, history panel, details tabs, and status markers
- Keep long transaction messages from widening the progress window
- Show plain package versions in the package table
- Reduce memory kept after installed package listing
- Improve Stop feedback while package queries are shutting down

* Sun May 10 2026 ErikMN <dnfui@proton.me> - 0.1.4-1
- Harden transaction service request ownership checks
- Improve transaction progress reporting during apply
- Split transaction and package table code by responsibility
- Add Fedora review and native dependency documentation

* Fri May 08 2026 ErikMN <dnfui@proton.me> - 0.1.3-1
- Fix upgradeable package actions and labels
- Show installed package details for upgradeable package rows
- Add regression tests for upgradeable package handling

* Wed May 06 2026 ErikMN <dnfui@proton.me> - 0.1.2-1
- Reduce memory retention after package queries and transactions
- Serialize libdnf Base access and teardown
- Improve changelog metadata loading for package details

* Fri May 01 2026 ErikMN <dnfui@proton.me> - 0.1.1-1
- Add upgrade-all support
- Add upgradable package listing
- Improve documentation and tests

* Thu Apr 30 2026 ErikMN <dnfui@proton.me> - 0.1.0-1
- First public test release
