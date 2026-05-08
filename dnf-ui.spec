Name:           dnf-ui
Version:        0.1.3
Release:        1%{?dist}
Summary:        GTK frontend for DNF5 with a privileged transaction service

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
BuildRequires:  pkgconfig(polkit-gobject-1)
BuildRequires:  pkgconfig(systemd)
BuildRequires:  systemd-rpm-macros

Requires:       polkit
%{?systemd_requires}

%description
DNF UI is a graphical frontend for Fedora package management built with GTK 4
and libdnf5. It supports browsing available and installed packages, reviewing
transaction changes, and applying package transactions through a privileged
system service with Polkit authorization.

%prep
%autosetup -p1

%build
%meson -Dbuild_tests=true
%meson_build

%install
%meson_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/com.fedora.dnfui.desktop
appstreamcli validate --no-net %{buildroot}%{_datadir}/metainfo/com.fedora.dnfui.metainfo.xml
%meson_test

%post
%systemd_post dnfui-service.service

%preun
%systemd_preun dnfui-service.service

%postun
%systemd_postun_with_restart dnfui-service.service

%files
%license LICENSE
%doc README.md
%{_bindir}/dnfui
%{_libexecdir}/dnfui-service
%{_datadir}/applications/com.fedora.dnfui.desktop
%{_datadir}/icons/hicolor/48x48/apps/com.fedora.dnfui.png
%{_datadir}/icons/hicolor/64x64/apps/com.fedora.dnfui.png
%{_datadir}/icons/hicolor/128x128/apps/com.fedora.dnfui.png
%{_datadir}/icons/hicolor/256x256/apps/com.fedora.dnfui.png
%{_datadir}/icons/hicolor/512x512/apps/com.fedora.dnfui.png
%{_datadir}/icons/hicolor/1024x1024/apps/com.fedora.dnfui.png
%{_datadir}/metainfo/com.fedora.dnfui.metainfo.xml
%{_datadir}/dbus-1/system-services/com.fedora.Dnfui.Transaction1.service
%{_datadir}/polkit-1/actions/com.fedora.dnfui.policy
%{_sysconfdir}/dbus-1/system.d/com.fedora.Dnfui.Transaction1.conf
%{_unitdir}/dnfui-service.service

%changelog
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
