# Development

This document covers local build, Docker, and RPM packaging commands for DNF UI.

## Native build

Fedora build dependencies are listed in
[fedora-native-dependencies.txt](fedora-native-dependencies.txt).

Install them with:

```sh
./utils/install_fedora_dependencies.sh
```

Meson handles the real build and install logic.
The `Makefile` is a thin task runner for the common developer commands.

Build and run:

```sh
make && ./dnfui
```

Build final and run:

```sh
FINAL=y make && ./dnfui
```

Run the Meson setup directly:

```sh
meson setup build/debug --prefix /usr --libexecdir libexec
meson compile -C build/debug
./build/debug/src/dnfui
```

## Native service install for development

For native Polkit testing from the source tree, install the service files with:

```sh
make
sudo make serviceinstall
```

Then run the app as a regular desktop user:

```sh
./dnfui
```

When you apply a transaction, the desktop Polkit prompt should appear.

Remove the development service install with:

```sh
sudo make serviceuninstall
```

**NOTE:**

- Choose a non critical installed package for native apply tests
- `serviceinstall` is just a development helper for installing the app without RPM packaging

## Docker

[Docker](https://www.docker.com/) is the default container runtime.
The container targets also support [Podman](https://podman.io/)
by setting `CONTAINER_RUNTIME=podman`. The target names still says Docker though.

Running the application in a container is useful for testing and developing
without affecting the host system.

Build the development image:

```sh
make dockersetup
```

Build the development image with Podman:

```sh
CONTAINER_RUNTIME=podman make dockersetup
```

Run the application in a container:

```sh
make dockerrun
```

Run the application in Docker with networking disabled:

```sh
make dockerrunoffline
```

Run the application in Docker with networking disabled and an empty repo cache:

```sh
make dockerruncoldoffline
```

## RPM packaging

Fedora RPM packaging is included for this application.

Build a source RPM from the current tracked working tree:

```sh
make srpm
```

Build binary and source RPMs locally:

```sh
make rpm
```

Build a source RPM in Docker:

```sh
make dockersrpm
```

Build binary and source RPMs in Docker:

```sh
make dockerrpm
```

Artifacts are written under `./rpmbuild/`.

Notes:

- The RPM package is built from `dnf-ui.spec`
- `make srpm` includes files tracked by Git in the generated source tarball
- The Docker RPM targets use the existing Fedora development image and write artifacts into the same `./rpmbuild/` tree

Run `rpmlint` on the source RPM and binary RPMs:

```sh
rpmlint dnf-ui-latest.src.rpm rpmbuild/RPMS/*/*.rpm
```

To check that the package builds without depending on files or packages from your
own system, rebuild the generated source RPM in an isolated Fedora build
environment with `mock`:

```sh
mock -r fedora-rawhide-x86_64 --rebuild dnf-ui-latest.src.rpm
```

Use a different `-r` value if you want to build for another Fedora release or architecture.
