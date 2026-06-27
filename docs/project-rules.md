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
  state shown in the UI.
- dnf5daemon is the source for transaction previews, apply work, and Upgrade All
  validation.
- The generic Status column may show that repository metadata contains a newer candidate. It must not be treated as proof that dnf5daemon can apply that upgrade.
- The List Upgradable view must be checked against the resolved dnf5daemon Upgrade All preview by package name and architecture before showing rows, because dnf5daemon is the service that applies package transactions. This check filters libdnf5 rows that dnf5daemon cannot upgrade. It must not become a strict equality check unless the app stops using libdnf5 as the table row source.
- If libdnf5 reports no upgrade rows but dnf5daemon resolves upgrades, show a clear error instead of a false empty list.
- When showing an upgradable package, details tabs should describe the currently
  installed package unless the text clearly says it is showing update information.
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
