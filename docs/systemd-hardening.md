# Systemd and transaction hardening

DNF UI no longer installs its own privileged transaction service.

Package changes are applied through DNF5 dnf5daemon. That means the systemd
unit, D-Bus policy, and Polkit behavior for package changes belong to the Fedora
DNF stack, not to this project.

## DNF UI responsibility

DNF UI still has security responsibilities:

- keep the GTK application unprivileged
- send package changes through dnf5daemon
- show the resolved transaction before apply
- close daemon sessions when they are no longer needed
- avoid hiding unsupported transaction actions from the user

## What this project does not ship

DNF UI does not install:

- a custom root-running transaction service
- a custom transaction Polkit policy
- a custom transaction D-Bus policy
- a custom systemd unit for package apply work

## What to check when packaging

When reviewing the package, verify that the RPM requires `dnf5daemon-server` and
does not install old DNF UI service files.

The files that should not be installed are:

- `dnfui-service`
- `com.fedora.Dnfui.Transaction1.service`
- `com.fedora.Dnfui.Transaction1.conf`
- `com.fedora.dnfui.policy`
- `dnfui-service.service`

System hardening for package apply work should be reviewed in DNF5's
dnf5daemon packaging. DNF UI should not duplicate that service.
