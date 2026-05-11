Yes. I think **dnf-ui could realistically mature into something acceptable for the Fedora repos**, assuming you keep maintaining it and polish the packaging/security edges.

It already has several things in its favor:

| Fedora inclusion factor | dnf-ui status                                                                                                                                     |
| ----------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| Fedora-native stack     | GTK4, libdnf5, Polkit, D-Bus, systemd. That fits Fedora well.                                                                                     |
| Build system            | Meson, normal Fedora RPM macros in the spec.                                                                                                      |
| License                 | MIT in the spec, which is Fedora-compatible if the repo contents match it.                                                                        |
| Desktop integration     | The spec installs a desktop file, icons, AppStream metadata, D-Bus service file, Polkit policy, and systemd service.                              |
| Tests                   | It has Catch2 unit tests, functional service smoke tests, Docker test helpers, offline tests, system bus tests, and Valgrind targets documented.  |
| Architecture clarity    | The docs explain the unprivileged GUI, libdnf5 backend, transaction service, Polkit boundary, and D-Bus request model.                            |

The biggest thing in its favor is that it does **not** look like a random toy GUI. It already has packaging, a transaction boundary, tests, and a clearly limited scope.

The biggest thing against it is not that it is early. Fedora can package young projects. The issue is that it is a **privileged package-management frontend**, so review expectations will be higher than for a normal desktop utility. Fedora package review requires things like correct licensing, no bundled system libraries, correct build dependencies, successful builds on supported architectures, and a valid desktop file for GUI applications. ([Fedora Project][1])

For dnf-ui, I think the review pressure points would be:

1. **Privileged service design**

   This is the main one. A reviewer will likely care about whether the D-Bus service has a narrow API, validates requests properly, does not allow arbitrary command execution, uses Polkit correctly, and behaves safely on cancellation or client disconnect. Your docs already help because the service boundary is explicit.

2. **Systemd and D-Bus packaging**

   Fedora has specific systemd packaging expectations. Unit files go into `%{_unitdir}`, systemd units must not be marked as config files, and D-Bus activation has its own packaging considerations. ([Fedora Project][2]) Your spec already uses `%{_unitdir}` and systemd scriptlets, which is a good start.

3. **Real upstream release hygiene**

   Versioned release tarballs, clean changelogs, reproducible source archives, no generated junk, no vendored libraries, and mock builds matter. Your spec currently shows version `0.1.3` and a normal changelog, so this is already moving in the right direction.

4. **Maturity of dangerous operations**

   Install/remove/update workflows need boring reliability. For repo inclusion, “fast and never crashes for me” is useful, but regression tests around transactions, cancellation, offline metadata, local-only packages, protected packages, and failure states are what make the argument stronger. Your testing document already covers many of these areas.

5. **Positioning versus existing tools**

   dnfdragora already exists in Fedora, and GNOME Software exists for app-store style workflows. But dnf-ui has a clear niche: a fast, Synaptic-like package manager frontend, not an app store. That is a defensible scope.

My honest judgement: **yes, it is plausible**. I would not submit it as “ready for everyone” just because it works well locally, but I do think it has the shape of a Fedora-packageable project.

Before trying a formal Fedora review, I would want these in place:

| Priority | Item                                                                                   |
| -------- | -------------------------------------------------------------------------------------- |
| High     | Clean `mock` build for Rawhide and current stable Fedora                               |
| High     | `rpmlint` output reviewed and either fixed or justified                                |
| High     | Polkit policy and D-Bus service interface reviewed carefully                           |
| High     | No unsafe shell execution in transaction paths                                         |
| High     | Tests for cancellation, failed transactions, invalid requests, and service disconnects |
| Medium   | AppStream metadata passes validation                                                   |
| Medium   | Desktop file passes validation                                                         |
| Medium   | Man page or at least clear CLI/help behavior                                           |
| Medium   | A Copr repo for wider testing before Fedora review                                     |
| Low      | More screenshots, user docs, and known-limitations section                             |

I would probably first publish it in **Copr**, get a few Fedora users to test it, collect bug reports, then submit a Fedora package review once it has survived normal daily use outside your own machines. That gives the package review a much stronger story.

[1]: https://fedoraproject.org/wiki/Packaging/ReviewGuidelines "Packaging:ReviewGuidelines - Fedora Project Wiki"
[2]: https://fedoraproject.org/wiki/Packaging%3ASystemd "Packaging:Systemd - Fedora Project Wiki"


Yes. After scanning more of the repo, I think several of the Fedora-readiness questions can already be answered.

My updated judgement: **dnf-ui is not just “maybe someday packageable”. It already has a fairly serious Fedora-packageable shape.** I would still not call it ready for review without a real `mock` build, `rpmlint`, and broader testing, but the repo is much further along than a typical early GUI project.

## What already looks good

| Question                      | Current answer                                                                                                                                                                                              |
| ----------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| License clean?                | Looks good. The repo has an MIT license file, and the spec says `License: MIT`.                                                                                                                             |
| Versioning consistent?        | Looks good. `meson.build` says version `0.1.3`, and the RPM spec also says `Version: 0.1.3`.                                                                                                                |
| Normal Fedora build style?    | Looks good. The spec uses `%meson`, `%meson_build`, `%meson_install`, and `%meson_test`, with Fedora-style `BuildRequires` for GTK4, libdnf5, Polkit, systemd, AppStream, desktop-file-utils, and Catch2.   |
| Desktop file present?         | Yes. It installs a normal visible desktop file with `Exec=dnfui`, icon, categories, startup notify, and keywords.                                                                                           |
| AppStream metadata present?   | Yes. It has AppStream metadata with component id, metadata license, project license, summary, description, launchable desktop id, binary, categories, and content rating.                                   |
| Desktop/AppStream validation? | Yes. The spec runs `desktop-file-validate` and `appstreamcli validate --no-net` in `%check`.                                                                                                                |
| RPM build helpers?            | Yes. The Makefile has `srpm`, `rpm`, `dockersrpm`, and `dockerrpm` targets. The SRPM script builds a tarball from `git ls-files`, which is a reasonable way to avoid untracked junk in local test builds.   |
| Tests?                        | Strong for an early project. Meson builds a Catch2 test binary with backend, search, offline, transaction preview, transaction request, and service-client tests.                                           |
| Privilege split?              | Good design direction. The GUI and service are separate executables. The service installs under libexec and is activated over D-Bus/systemd.                                                                |
| Polkit boundary?              | Good. Apply authorization is only skipped on the session bus development/test path. On the system bus, it checks Polkit before starting package changes.                                                    |
| Transaction review enforced?  | Good. `Apply` refuses to run unless preview has succeeded and is non-empty.                                                                                                                                 |
| State changed after preview?  | Good. Apply resolves the transaction again and rejects it if the resolved preview differs from what the user approved. That is an important safety property.                                                |
| Request validation?           | Good. Empty requests, too many items, oversized specs, empty specs, and mixed upgrade-all requests are rejected. The tests cover these rules.                                                               |
| Service abuse limits?         | Some protection exists. There are global live request limits, per-client request limits, and a max preview worker count.                                                                                    |
| Client disconnect handling?   | Good sign. The service watches client ownership on the system bus and auto-releases orphaned sessions. There is also a system bus disconnect smoke test.                                                    |
| Self-removal protection?      | Good. The service rejects remove/reinstall requests for the package owning the running application.                                                                                                         |

## Things I would expect Fedora review to question

The main weak point is the **system bus D-Bus policy**. It allows any default-context client to send messages to `com.fedora.Dnfui.Transaction1`. Apply is still protected by Polkit, but preview requests can be triggered by any local user. There are request and worker limits, so this is not obviously broken, but a reviewer may ask whether preview should also require authorization, or whether the D-Bus policy should be narrower.

The systemd service is very simple: root, D-Bus type, restart on failure. That is acceptable as a starting point, but because this is a privileged package transaction service, I would expect questions about hardening. Some hardening may be difficult because package management needs broad system access, but the current unit has no hardening directives at all.

I did not see evidence that `mock` or `rpmlint` has been run. Fedora review explicitly expects `rpmlint` output to be posted, and mock builds are a normal reviewer expectation. ([Fedora Project][1])

There is no man page that I saw. That is usually a SHOULD, not a hard blocker, but it is still something reviewers may mention. Fedora’s review guidelines list man pages for binaries/scripts as a SHOULD item. ([Fedora Project][1])

The project has gettext setup, but I did not verify installed locale files. If translations are added later, the spec will need to handle locales correctly, normally with `%find_lang`. Fedora review guidelines call proper locale handling a MUST when applicable. ([Fedora Project][1])

## My answer to the earlier checklist

| Item                                  | Can answer now? | Result                                                                                                                                 |
| ------------------------------------- | --------------: | -------------------------------------------------------------------------------------------------------------------------------------- |
| Clean mock build                      |              No | Needs to be run outside repo scan                                                                                                      |
| rpmlint reviewed                      |              No | Needs to be run                                                                                                                        |
| License/spec consistency              |             Yes | Looks good                                                                                                                             |
| Desktop file validation               |             Yes | Built into `%check`                                                                                                                    |
| AppStream validation                  |             Yes | Built into `%check`                                                                                                                    |
| Polkit policy present                 |             Yes | Looks reasonable, but security-sensitive                                                                                               |
| D-Bus service files present           |             Yes | Present and installed                                                                                                                  |
| Systemd unit present                  |             Yes | Present, but minimal                                                                                                                   |
| Narrow privileged API                 |             Yes | Looks fairly narrow                                                                                                                    |
| Request validation                    |             Yes | Good                                                                                                                                   |
| Preview-before-apply                  |             Yes | Good                                                                                                                                   |
| Apply rechecks approved transaction   |             Yes | Very good                                                                                                                              |
| Tests for validation/service behavior |             Yes | Good for early stage                                                                                                                   |
| Obvious shell execution risk          |          Partly | I searched for common direct process execution patterns and did not see obvious runtime shell execution, but this was not a full audit |

## Bottom line

Yes, a repo scan already answers a lot.

I would say **dnf-ui is plausibly Fedora-packageable after hardening and packaging review work**, not merely after “more time passes”.

The strongest argument for it is not speed. It is this combination:

* normal Fedora RPM structure
* unprivileged GUI
* small privileged D-Bus service
* Polkit on apply
* explicit transaction preview
* revalidation before apply
* documented architecture
* real tests for request validation and service behavior

The things I would fix or prepare before submitting to Fedora are:

1. Run `mock` for Rawhide and at least current stable Fedora.
2. Run `rpmlint` on SRPM and binary RPMs and document the output.
3. Decide whether unauthenticated preview over the system bus is acceptable.
4. Add at least basic systemd hardening where it does not break package operations.
5. Add a short SECURITY.md explaining the privilege boundary.
6. Add a man page or justify not having one.
7. Put the result in Copr first and get real external testers.

If those are handled, I think a Fedora package review would be a serious conversation, not a long shot.

[1]: https://fedoraproject.org/wiki/Packaging/ReviewGuidelines "Packaging:ReviewGuidelines - Fedora Project Wiki"
