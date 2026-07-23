# Project rules

This file states the rules that should stay true as DNF UI changes.

**AI tools** can help with the project, but humans **MUST** be able to **understand**,
**review**, **test**, and **explain** the code that is merged.

## Human control

- Every important behavior should be explainable **without** asking an AI tool.
- Small focused patches are preferred over large rewrites.
- Do **NOT** add clever abstractions unless they remove real confusion.
- Comments should explain intent, boundaries, or non-obvious behavior.
- Tests should describe important user-visible rules, not private implementation
  details.
- If code cannot be explained, it should be reviewed, documented, simplified, or removed.

## Privilege boundary

- The main GTK application stays unprivileged.
- The UI must **never** apply package changes directly.
- Package apply work **must** go through dnf5daemon.
- Polkit authorization belongs at the privileged apply boundary.
- DNF UI must validate requests before sending them to dnf5daemon.
- Preview and apply **must** stay separate so the user can review the transaction
  before changing the system.

## Package state rules

- Package rows should be plain values that the UI can display without knowing
  libdnf5 internals.
- An upgradable package row represents an installed package with a newer
  repository candidate.
- libdnf5 is the source for package rows, package details, and local package
  state shown in the UI. For List Upgradable rows, the displayed update target
  comes from dnf5daemon.
- dnf5daemon is the source for transaction previews, apply work, Upgrade All,
  and the List Upgradable upgrade target set.
- The generic Status column may show that repository metadata contains a newer candidate. It must not be treated as proof that dnf5daemon can apply that upgrade.
- The List Upgradable view must start from dnf5daemon upgrade targets. libdnf5 may add display metadata, but it must not add extra upgrade rows that the daemon did not report.
- If dnf5daemon reports an upgrade whose metadata cannot be loaded from libdnf5, keep a basic daemon row visible instead of hiding the upgrade.
- When showing an upgradable package, details tabs should describe the currently
  installed package unless the text clearly says it is showing update information.
- For daemon-backed List Upgradable rows, the Info tab must use the attached
  daemon target for update information instead of selecting a separate libdnf5
  candidate.
- Installed package checks must not load repository metadata when local rpmdb
  state is enough.
- Repository refresh can update repository-visible package state, but it should
  **not** pretend to install or remove packages.

## libdnf5 access

- GTK code should **never** use libdnf5 types directly.
- libdnf5 access **must** stay behind the backend API or the transaction client.
- Shared Base access **must** stay serialized.
- A system-only Base should be used for installed-only checks.
- Dropping the cached Base is a memory choice and should be reviewed when query
  speed or memory use changes.
- Base generation changes matter because cached query results depend on them.
- Search cache entries are bounded UI snapshots. They may survive shared Base
  memory release, but known package-state changes must explicitly clear them.

## Transactions

- Transaction preview **must** show the package actions the user is about to approve.
- Apply **must** re-resolve the transaction and reject it if the approved action set
  changed.
- The preview comparison should compare the same package action set, not resolver
  iteration order.
- Transaction progress should show **useful** user-facing stages without becoming
  a debug log.
- The UI **must** refresh package state after a successful apply.
- Failed or cancelled transactions **must** leave the UI in a state where the user
  can try again.

## UI behavior

- Slow package work **must not** run on the GTK thread.
- The UI should stay **responsive** during search, preview, apply, and repository
  refresh.
- Stop and cancel wording **must** match what the app can actually interrupt.
- The table, details tabs, context menu, and pending actions **must** agree about
  what action is available for a selected package.
- UI polish is welcome when it **improves** clarity, click targets, or readability.

## Review checklist

Before merging a non-trivial change, check:

- Can a maintainer **explain** what changed and why?
- Does this keep the privilege boundary intact?
- Does this keep libdnf5 behind the backend or service boundary?
- Does this avoid blocking the GTK thread?
- Does this preserve installed-only behavior where repository metadata is not needed?
- Does this need a focused test for an important rule?
- Does this change make the code easier for a **human** to follow?
