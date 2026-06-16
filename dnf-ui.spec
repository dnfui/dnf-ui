Name:           dnf-ui
Version:        0.2.2
Release:        1%{?dist}
Summary:        GTK frontend for DNF5

License:        MIT
URL:            https://github.com/ErikMN/dnf-ui
Source0:        %{name}-%{version}.tar.gz

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
DNF UI is a graphical frontend for Fedora package management built with GTK 4
and libdnf5. It supports browsing available and installed packages, reviewing
transaction changes, and applying package transactions through Fedora
dnf5daemon.

%prep
%autosetup -p1

%build
%meson -Dbuild_tests=true
%meson_build

%install
%meson_install
%find_lang %{name}

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/com.fedora.dnfui.desktop
appstreamcli validate --no-net %{buildroot}%{_datadir}/metainfo/com.fedora.dnfui.metainfo.xml
%meson_test

%files -f %{name}.lang
%license LICENSE
%doc README.md
%{_bindir}/dnfui
%{_datadir}/applications/com.fedora.dnfui.desktop
%{_datadir}/icons/hicolor/48x48/apps/com.fedora.dnfui.png
%{_datadir}/icons/hicolor/64x64/apps/com.fedora.dnfui.png
%{_datadir}/icons/hicolor/128x128/apps/com.fedora.dnfui.png
%{_datadir}/icons/hicolor/256x256/apps/com.fedora.dnfui.png
%{_datadir}/icons/hicolor/512x512/apps/com.fedora.dnfui.png
%{_datadir}/icons/hicolor/1024x1024/apps/com.fedora.dnfui.png
%{_datadir}/metainfo/com.fedora.dnfui.metainfo.xml

%changelog
* Tue Jun 16 2026 ErikMN <erik@example.invalid> - 0.2.2-1
- Add repository signing key approval during transactions
- Show live repository refresh progress
- Improve cancellation handling for search and selected-package reloads
- Harden dnf5daemon preview parsing and resolve result handling
- Keep exact package reloads off the GTK thread

* Sat Jun 13 2026 ErikMN <erik@example.invalid> - 0.2.1-1
- Force repository metadata refresh when using the Refresh repositories button
- Allow repository refresh to be stopped from the UI while the current repo check finishes
- Improve transaction safety around dnf5daemon session handling
- Use dnf5daemon's native Upgrade All handling
- Block transactions that would remove dnf5daemon-server
- Prevent Upgrade All from upgrading DNF UI itself
- Avoid periodic installed-package refresh while a transaction is applying
- Fix failed preview session cleanup

* Mon Jun 08 2026 ErikMN <erik@example.invalid> - 0.2.0-1
- Use DNF5 dnf5daemon for transaction preview and apply
- Remove the custom privileged transaction service
- Add dnf5daemon transaction tests and native apply test target
- Improve stale-preview apply failures
- Block removal of dnf5daemon-server from DNF UI

* Tue Jun 02 2026 ErikMN <erik@example.invalid> - 0.1.7-1
- Cache improvements
- Search improvements
- Search timer
- UI polish

* Wed May 27 2026 ErikMN <erik@example.invalid> - 0.1.6-1
- Make transaction preview fail closed for unsupported resolved actions
- Split test-only transaction service hooks out of the installed service
- Authorize system-bus preview requests before privileged preview work starts
- Fix search cache reuse after the shared backend Base is dropped and recreated
- Improve transaction service test coverage and supporting documentation

* Fri May 15 2026 ErikMN <erik@example.invalid> - 0.1.5-1
- Polish the package list, history panel, details tabs, and status markers
- Keep long transaction messages from widening the progress window
- Show plain package versions in the package table
- Reduce memory kept after installed package listing
- Improve Stop feedback while package queries are shutting down

* Sun May 10 2026 ErikMN <erik@example.invalid> - 0.1.4-1
- Harden transaction service request ownership checks
- Improve transaction progress reporting during apply
- Split transaction and package table code by responsibility
- Add Fedora review and native dependency documentation

* Fri May 08 2026 ErikMN <erik@example.invalid> - 0.1.3-1
- Fix upgradeable package actions and labels
- Show installed package details for upgradeable package rows
- Add regression tests for upgradeable package handling

* Wed May 06 2026 ErikMN <erik@example.invalid> - 0.1.2-1
- Reduce memory retention after package queries and transactions
- Serialize libdnf Base access and teardown
- Improve changelog metadata loading for package details

* Fri May 01 2026 ErikMN <erik@example.invalid> - 0.1.1-1
- Add upgrade-all support
- Add upgradable package listing
- Improve documentation and tests

* Thu Apr 30 2026 ErikMN <erik@example.invalid> - 0.1.0-1
- First public test release
