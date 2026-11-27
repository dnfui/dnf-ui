// -----------------------------------------------------------------------------
// test/unit/test_tui_pending_actions.cpp
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "test_utils.hpp"
#include "dnf_backend/dnf_internal.hpp"
#include "tui/pending_actions.hpp"

namespace {

PackageRow
make_row(const char *version)
{
  PackageRow row;
  row.name = "demo";
  row.arch = "noarch";
  row.epoch = "0";
  row.version = version;
  row.release = "1";
  row.nevra = row.name + "-" + row.version + "-1.noarch";
  return row;
}

void
publish_installed(const PackageRow &row, bool self_protected = false)
{
  dnf_backend_internal::InstalledQueryResult installed;
  installed.rows = { row };
  installed.nevras = { row.nevra };
  installed.rows_by_name_arch.emplace(row.name_arch_key(), row);
  std::set<std::string> protected_names;
  if (self_protected) {
    protected_names.insert(row.name);
  }
  dnf_backend_internal::publish_installed_snapshot(std::move(installed), std::move(protected_names));
}

} // namespace

TEST_CASE("TUI marks available packages by exact NEVRA")
{
  reset_backend_globals();
  std::vector<tui::PendingMark> actions;
  const tui::DisplayRow row { make_row("1"), false };
  REQUIRE_FALSE(tui::mark_pending_action(actions, row, PendingAction::REMOVE));
  REQUIRE(tui::mark_pending_action(actions, row, PendingAction::INSTALL));
  REQUIRE(tui::mark_pending_action(actions, row, PendingAction::INSTALL));
  REQUIRE(actions.size() == 1);
  REQUIRE(actions.front().action.transaction_spec == row.package.nevra);
  REQUIRE(actions.front().action.type == PendingAction::INSTALL);
}

TEST_CASE("TUI replaces conflicting actions on the same installed row")
{
  reset_backend_globals();
  auto installed = make_row("1");
  installed.repo_candidate_exact_available = true;
  publish_installed(installed);
  const tui::DisplayRow row { installed, true };
  std::vector<tui::PendingMark> actions;
  REQUIRE_FALSE(tui::mark_pending_action(actions, row, PendingAction::INSTALL));
  REQUIRE(tui::mark_pending_action(actions, row, PendingAction::REMOVE));
  REQUIRE(tui::mark_pending_action(actions, row, PendingAction::REINSTALL));
  REQUIRE(actions.size() == 1);
  REQUIRE(actions.front().action.type == PendingAction::REINSTALL);
  REQUIRE(actions.front().action.transaction_spec == installed.nevra);
}

TEST_CASE("TUI blocks removal and reinstall of the running application")
{
  reset_backend_globals();
  auto installed = make_row("1");
  installed.repo_candidate_exact_available = true;
  publish_installed(installed, true);
  const tui::DisplayRow row { installed, true };
  std::vector<tui::PendingMark> actions;
  REQUIRE_FALSE(tui::mark_pending_action(actions, row, PendingAction::REMOVE));
  REQUIRE_FALSE(tui::mark_pending_action(actions, row, PendingAction::REINSTALL));
  REQUIRE(actions.empty());
  reset_backend_globals();
}

TEST_CASE("TUI upgrade marking requires a current daemon target")
{
  reset_backend_globals();
  publish_installed(make_row("1"));
  auto &state = DaemonUpgradeState::instance();
  state.reset_for_tests();
  auto available = make_row("2");
  available.is_newest_available = true;
  tui::DisplayRow row { available, false };
  std::vector<tui::PendingMark> actions;
  REQUIRE_FALSE(tui::mark_pending_action(actions, row, PendingAction::UPGRADE));

  DaemonUpgradeTarget target;
  target.name = available.name;
  target.arch = available.arch;
  target.epoch = available.epoch;
  target.version = available.version;
  target.release = available.release;
  target.nevra = available.nevra;
  target.full_nevra = "demo-0:2-1.noarch";
  const auto refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());
  std::string error;
  REQUIRE(state.publish_success(*refresh_id, { target }, error) == DaemonUpgradePublishResult::PUBLISHED);
  row.upgrade =
      std::make_shared<tui::UpgradeRowContext>(tui::UpgradeRowContext { target, state.snapshot().generation });
  REQUIRE(tui::mark_pending_action(actions, row, PendingAction::UPGRADE));
  REQUIRE(actions.size() == 1);
  REQUIRE(actions.front().action.transaction_spec == "demo.noarch");
  REQUIRE(actions.front().action.nevra == target.nevra);
  REQUIRE(actions.front().source.upgrade->target.full_nevra == target.full_nevra);
  REQUIRE(tui::pending_snapshot_issue(actions.front(), state.snapshot()).empty());

  const auto next_refresh = state.begin_refresh();
  REQUIRE(next_refresh.has_value());
  REQUIRE(state.publish_success(*next_refresh, { target }, error) == DaemonUpgradePublishResult::PUBLISHED);
  REQUIRE(tui::pending_snapshot_issue(actions.front(), state.snapshot()).empty());

  const auto changed_refresh = state.begin_refresh();
  REQUIRE(changed_refresh.has_value());
  auto changed_target = target;
  changed_target.version = "3";
  changed_target.nevra = "demo-3-1.noarch";
  changed_target.full_nevra = "demo-0:3-1.noarch";
  REQUIRE(state.publish_success(*changed_refresh, { changed_target }, error) == DaemonUpgradePublishResult::PUBLISHED);
  REQUIRE(tui::pending_snapshot_issue(actions.front(), state.snapshot()) == "Invalid: daemon upgrade target changed.");
  REQUIRE(actions.front().source.upgrade->target.full_nevra == target.full_nevra);

  state.mark_stale();
  REQUIRE_FALSE(tui::mark_pending_action(actions, row, PendingAction::UPGRADE));
  REQUIRE(actions.size() == 1);
  REQUIRE(tui::pending_snapshot_issue(actions.front(), state.snapshot()) ==
          "Unchecked: current daemon targets are needed.");
  state.reset_for_tests();
  reset_backend_globals();
}

TEST_CASE("TUI preserves removal marks when installed state changes")
{
  reset_backend_globals();
  const auto installed = make_row("1");
  publish_installed(installed);
  std::vector<tui::PendingMark> marks;
  REQUIRE(tui::mark_pending_action(marks, { installed, true }, PendingAction::REMOVE));
  REQUIRE(tui::pending_snapshot_issue(marks.front(), DaemonUpgradeState::instance().snapshot()).empty());

  publish_installed(make_row("2"));
  REQUIRE(tui::pending_snapshot_issue(marks.front(), DaemonUpgradeState::instance().snapshot()) ==
          "Invalid: marked version is no longer installed.");
  REQUIRE(marks.front().action.nevra == installed.nevra);
  reset_backend_globals();
}

TEST_CASE("TUI detects an externally installed pending package")
{
  reset_backend_globals();
  const auto available = make_row("1");
  std::vector<tui::PendingMark> marks;
  REQUIRE(tui::mark_pending_action(marks, { available, false }, PendingAction::INSTALL));

  publish_installed(available);
  REQUIRE(tui::pending_snapshot_issue(marks.front(), DaemonUpgradeState::instance().snapshot()) ==
          "Invalid: package is already installed.");
  REQUIRE(marks.front().action.type == PendingAction::INSTALL);
  reset_backend_globals();
}
