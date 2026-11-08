# DNF UI

<p align="center">
  <img src="img/logo.png" width="220" alt="DNF UI logo"/>
</p>

[![CI](https://github.com/dnfui/dnf-ui/actions/workflows/ci.yml/badge.svg)](https://github.com/dnfui/dnf-ui/actions/workflows/ci.yml)
[![ShellCheck](https://github.com/dnfui/dnf-ui/actions/workflows/shellcheck.yml/badge.svg)](https://github.com/dnfui/dnf-ui/actions/workflows/shellcheck.yml)
[![Release RPM](https://github.com/dnfui/dnf-ui/actions/workflows/release-rpm.yml/badge.svg)](https://github.com/dnfui/dnf-ui/actions/workflows/release-rpm.yml)
[![Publish COPR](https://github.com/dnfui/dnf-ui/actions/workflows/publish-copr.yml/badge.svg)](https://github.com/dnfui/dnf-ui/actions/workflows/publish-copr.yml)

**DNF UI** is a graphical frontend for DNF5 (Dandified YUM), inspired in part by [Synaptic](https://github.com/mvo5/synaptic).
It is built with [GTK 4](https://docs.gtk.org/gtk4/) and [libdnf5](https://github.com/rpm-software-management/dnf5) and
aims to provide a **fast** and **dependable** package management workflow for DNF5 systems.

## Supported platform

DNF UI targets systems using modern **DNF5** and **dnf5daemon**. It is developed and tested primarily on Fedora Linux.
The Fedora RPM declares a runtime dependency on `dnf5daemon-server`.

## Status

DNF UI is under active development and is stable for regular use. \
New features and improvements are still being added, so some interfaces and
behavior may change slightly between releases.

## Install from COPR

DNF UI is available from [COPR](https://copr.fedorainfracloud.org/coprs/):

<https://copr.fedorainfracloud.org/coprs/erikmn/dnf-ui/>

Enable the repository and install the app:

```sh
sudo dnf install dnf5-plugins
sudo dnf copr enable erikmn/dnf-ui
sudo dnf install dnf-ui
```

## Goals and principles

- User experience first!
- Reliable and fast
- Strong focus on code quality and maintainability
- No unnecessary complexity or bloat

## Scope

DNF UI is a package manager frontend, **NOT** an app store.

It focuses on fast package search, package details, installed package inspection,
repository management, explicit transaction review, and applying DNF package
transactions through Polkit.

DNF UI does **NOT** aim to manage Flatpaks, firmware updates, ratings, featured
applications, or software-center discovery workflows.
Applications such as [GNOME Software](https://apps.gnome.org/Software/) cover those workflows.

## Current features

- Find and browse packages
- Inspect package information
- Install, uninstall, reinstall, downgrade or upgrade packages
- Enable or disable repositories
- Browse transaction history
- Export to CSV

## Why?

As a long-time user of Synaptic, I wanted a similar tool for DNF5:
a native package manager frontend that is fast, reliable, and easy to use.

This project is also a practical way for me to learn more about building a stable and maintainable desktop application.
The goal is not experimentation for its own sake, but to build something genuinely useful for me and others.

Other good graphical package managers targeting DNF5 include:

- [dnfdragora](https://github.com/manatools/dnfdragora)
- [yumex-ng](https://github.com/timlau/yumex-ng)

## Contributing

See [docs/contributing.md](docs/contributing.md).

## Development

Fedora build dependencies are listed in
[docs/fedora-native-dependencies.txt](docs/fedora-native-dependencies.txt).

Install them, then build and run:

```sh
./utils/install_fedora_dependencies.sh
make && ./dnfui
```

Run the native test suite:

```sh
make test
```

- Developer documentation: [docs/development.md](docs/development.md).
- Test documentation: [docs/testing.md](docs/testing.md).
- Architecture documentation: [docs/architecture.md](docs/architecture.md).

## Screenshots

<p align="center">
  <img src="img/latest_screenshot.png" width="900" alt="DNF UI GNOME screenshot"/>
</p>

<p align="center">
Dark theme.
</p>

<p align="center">
  <img src="img/latest_screenshot2.png" width="900" alt="DNF UI KDE screenshot"/>
</p>

<p align="center">
Light theme.
</p>
