# Development

This document covers local build, Docker, and RPM packaging commands for DNF UI.

## Native build

Fedora build dependencies are listed in [fedora-native-dependencies.txt](fedora-native-dependencies.txt).

Install them with:

```sh
./utils/install_fedora_dependencies.sh
```

Meson handles the real build and install logic.
The `Makefile` is a thin task runner for the common developer commands.

To use the project Git hooks in this checkout:

```sh
make setup-hooks
```

To fetch upstream DNF5 source for local API reference:

```sh
make dnf5-source
```

The checkout is written to `external/dnf5`, which is ignored by Git.

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

## Backend CLI probe

The source tree includes a small developer CLI for testing the backend without starting the GTK application.
It is not installed by the RPM.

Build it natively:

```sh
make dnfui-backend-cli
./dnfui-backend-cli search bash
```

Open a Docker shell with the CLI built and ready:

```sh
make dockercli
```

The Docker CLI target builds under `/tmp/dnfui-build`, starts a container system bus for dnf5daemon commands,
and opens a shell with `dnfui-backend-cli` available.

Read-only libdnf commands:

```sh
dnfui-backend-cli search bash
dnfui-backend-cli list-installed bash
dnfui-backend-cli list-packages --all-versions
NEVRA="$(dnfui-backend-cli search --exact bash | awk 'NR == 1 { print $1 }')"
dnfui-backend-cli details "$NEVRA"
```

Daemon-backed probes:

```sh
dnfui-backend-cli list-upgrades
dnfui-backend-cli repos list
dnfui-backend-cli repos enable REPO_ID
dnfui-backend-cli repos disable REPO_ID
dnfui-backend-cli preview install cowsay
dnfui-backend-cli preview-upgrade-all
```

The CLI only previews transactions. It does not apply package changes.
Repository enable and disable commands do change system repository configuration through dnf5daemon.
The GUI self-protection check is process-specific, so the development CLI does not represent the GUI rule that blocks
DNF UI from removing or replacing itself.

## Experimental terminal frontend

The read-only terminal frontend uses the same package backend as **DNF UI**, but is built as a separate executable.
It is not installed by the RPM. Install `ncurses-devel` if your native development dependencies predate this target.

```sh
make run-tui
```

To run against the development container's package database, rebuild the image and launch the TUI:

```sh
make dockersetup
make dockertui
```

The container builds under `/tmp/dnfui-build` and starts its own system bus for dnf5daemon activation.
It does not mount the host package database or system bus.
If your terminal type is unavailable in the image, use `TERM=xterm-256color make dockertui`.

Use `/` to search, `i` to list installed packages, `a` to list all packages, `u` to list unneeded packages,
and `g` to list upgradable packages.
Press `r` to refresh repository metadata and reload the current view. Use the arrow and page keys to navigate,
Enter to open details, Escape to return, and `q` to quit. The terminal must be at least 50 columns wide and 8 rows high.
The selected package summary is shown when the terminal has at least 12 rows. Enter opens the full details.
The All view uses the same compact package listing as **DNF UI** (latest candidate per name and architecture).
The Unneeded view lists packages identified by libdnf5 as unneeded; it does not remove any packages.
It reloads DNF system state each time, following normal repository metadata expiration rules.
Package transactions and repository changes are not part of this proof of concept.

The Upgradable view requires dnf5daemon and uses its upgrade target list, not local version comparisons.
Its details show the reported target and summary, not a transaction preview.
The other views report whether the exact package version is installed. Refresh checks both daemon and local metadata.
If daemon refresh fails, local views still reload with a warning; the Upgradable view reports an error.
Backend warnings are printed after the terminal closes. To see them while the app runs, redirect stderr to a file.
Quitting cancels package queries where supported and waits for any non-cancellable backend work to finish.

For a manual smoke test, search for `bash` twice, refresh the current search with `r`, and inspect installed package details.
Switch between all five views, refresh each view and check the selected package summary and details.
Compare the Upgradable targets with `dnfui-backend-cli list-upgrades` on the same system.
When dnf5daemon is unavailable, check that local refresh still works with a warning and Upgradable reports an error.
`make dockerdnf5daemontest` covers a known TUI upgrade target after refresh and an unavailable daemon.
Resize the terminal, quit during a repository query and verify the shell is restored. Repeat in Docker.
Run with repositories unavailable to check the fallback notice and saved warnings. Run with redirected stdin or stdout
to verify that it rejects non-interactive use.

On a disposable Fedora system, open the Unneeded view and choose a listed dependency. In another terminal, run
`sudo dnf5 mark user PACKAGE`, replacing `PACKAGE` with its name. Press `u` again without using `r` and verify that
it is no longer listed. Restore its original dependency reason with `sudo dnf5 mark dependency PACKAGE` if it was
originally marked as a dependency, then reopen the view and check that it returns. Do not run this check on the host
unless changing installation reasons there is intended.

## Native transaction testing

For native Polkit testing from the source tree, build the app and run it as a regular desktop user:

```sh
make
./dnfui
```

dnf5daemon handles authorization when you apply a transaction.
A desktop Polkit prompt may appear if authentication is required and an authentication agent is available.

**NOTE:**

- Choose a non critical installed package for native apply tests
- Package changes go through DNF5 dnf5daemon

## Docker

[Docker](https://www.docker.com/) is the default container runtime.
The container targets also support [Podman](https://podman.io/) by setting `CONTAINER_RUNTIME=podman`.
The target names still say Docker though.

Running the application in a container is useful for testing and developing without affecting the host system.

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

## Heaptrack

On Fedora, install Heaptrack and record heap allocations locally as a regular user:

```sh
sudo dnf install heaptrack
make heaptrack
```

To record using the development container:

```sh
make dockersetup
make dockerheaptrack
```

Both targets use a debug build with AddressSanitizer disabled. Docker uses the normal GUI and dnf5daemon setup.
Native profiling uses the host repositories and system daemon.
Reproduce the workload, then close the application normally to finish the recording.

Each run writes a compressed Heaptrack recording and `report.txt` under `build/heaptrack/run.*`.
The report includes peak allocations, allocation counts, temporary allocations, and allocations still live at exit.
For a graphical analysis, open the compressed recording in `heaptrack_gui` on the host.

Heaptrack adds runtime overhead, so measure normal query timings and RSS separately with `make run` or `make dockerrun`.

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

To check that the package builds without depending on files or packages from your own system, rebuild the
generated source RPM in an isolated Fedora build environment with `mock`:

```sh
mock -r fedora-rawhide-x86_64 --rebuild dnf-ui-latest.src.rpm
```

Use a different `-r` value if you want to build for another Fedora release or architecture.

## Docker GDB

Run the app under GDB in the development container:

```sh
make dockergdb
```

Stop the GDB container if it is still running:

```sh
make dockergdbstop
```

The Docker GDB target starts a system bus in the container and checks that dnf5daemon can be activated
before launching GDB.
