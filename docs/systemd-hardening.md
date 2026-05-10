# Systemd hardening

DNF UI uses a small system service for package transactions. The desktop
application runs as the normal user, but the transaction service runs as root so
it can ask libdnf5 and rpm to install, remove, and upgrade packages.

This is a sensitive boundary. The service should stay small, have a narrow
D-Bus API, require Polkit before applying package changes, and validate requests
before libdnf5 work starts.

## Current unit

The service unit is intentionally simple:

```ini
[Service]
Type=dbus
BusName=com.fedora.Dnfui.Transaction1
ExecStart=/usr/libexec/dnfui-service --system
User=root
Restart=on-failure
RestartSec=1
```

This does not mean hardening was ignored. It means hardening must be chosen
carefully because the service performs package-manager work.

## Why aggressive sandboxing is risky

Systemd sandboxing applies to the service process and to child processes it
starts. During package transactions, rpm scriptlets and helper programs may run
inside the same service context.

That means a hardening option can break package installation even if the DNF UI
service binary itself still starts correctly.

The service is expected to modify the installed operating system. It may need to:

- write under `/usr`, `/etc`, `/var`, and other system paths
- install files with special permissions
- run package scriptlets
- use the network for package downloads
- talk to local system services over D-Bus or Unix sockets
- use rpm, libdnf5, compression tools, and package helper programs

For this reason, the unit should not copy generic hardening settings from a
normal network daemon.

## Options we should not add blindly

These options may be useful for many services, but they are risky for a package
transaction service:

- `ProtectSystem=`
  Makes system paths read-only. This conflicts with package installation and
  removal, which must modify the operating system.

- `ProtectHome=`
  Hides or restricts home directories. Package transactions should not normally
  touch user homes, but blocking access here can still surprise package
  scriptlets or helper tools. Do not add without native testing.

- `NoNewPrivileges=yes`
  Prevents gaining new privileges through executed programs. This may break
  package scriptlets or helper tools that rely on normal system behavior.

- `RestrictSUIDSGID=yes`
  Blocks setting setuid and setgid bits. Packages are allowed to install files
  with those bits when needed.

- `PrivateNetwork=yes`
  Blocks normal network access. libdnf5 needs network access when packages or
  metadata must be downloaded.

- `CapabilityBoundingSet=`
  Limits Linux capabilities. The safe capability set for arbitrary package
  transactions is not obvious. Narrowing it without proof risks breaking package
  scriptlets.

- `SystemCallFilter=`
  Blocks selected system calls. This is too broad a risk for rpm and package
  scriptlets unless each filter is tested carefully.

- `PrivateDevices=yes`
  Replaces the visible `/dev` tree. This may break package helper tools that
  expect normal device access.

- `RestrictFileSystems=`
  Limits filesystem types. A package manager should not assume which filesystems
  the installed system uses.

- `PrivateMounts=yes`
  Gives the service a private mount namespace. Package operations must affect
  the real system, so mount isolation needs very careful testing.

## Options that might be safe later

These options look less likely to break normal package transactions, but they
still need real testing before being added:

- `ProtectKernelLogs=yes`
- `ProtectClock=yes`
- `RestrictRealtime=yes`
- `LockPersonality=yes`
- `SystemCallArchitectures=native`

Do **not** add them only to improve the output of `systemd-analyze security`. Add
them only if they are proven not to break realistic package operations.

## Required testing before hardening changes

Any change to the service unit should be tested with real package transactions
on a disposable Fedora system.

At minimum, test:

- preview and apply for a normal install
- preview and apply for a removal
- preview and apply for a reinstall
- upgrade-all preview and apply
- failed transaction handling
- service restart and D-Bus activation
- Polkit authorization on the system bus
- package transactions that run rpm scriptlets

Docker tests are useful, but they are not enough for this decision. Native Fedora
testing matters because systemd, Polkit, rpm, scriptlets, SELinux, and the host
filesystem are part of the behavior.

## Current position

The current position is conservative:

- keep the privileged service small
- keep the D-Bus API narrow
- require Polkit before applying changes
- recheck the approved transaction before apply
- avoid systemd restrictions that may break package management

This is intentional. For this service, a short and predictable unit is safer than
a long hardening list that has not been proven against real package transactions.
