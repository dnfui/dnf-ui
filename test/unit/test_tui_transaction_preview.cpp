// -----------------------------------------------------------------------------
// test/unit/test_tui_transaction_preview.cpp
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "tui/transaction_preview.hpp"

namespace {

tui::PendingMark
upgrade_mark()
{
  tui::PendingMark mark {};
  mark.action = { PendingAction::UPGRADE, "demo-2-1.noarch", "demo.noarch", "demo\nnoarch" };
  return mark;
}

} // namespace

TEST_CASE("TUI transaction preview rejects substituted and missing marked upgrades")
{
  const std::vector<tui::PendingMark> marks { upgrade_mark() };
  TransactionPreview preview;
  preview.upgrade.push_back({ "demo-3-1.noarch", "demo", "noarch" });
  REQUIRE_FALSE(tui::preview_mark_issues(preview, marks, {}).front().empty());

  preview.upgrade.front().label = "demo-2-1.noarch";
  REQUIRE(tui::preview_mark_issues(preview, marks, {}).front().empty());

  preview.install = preview.upgrade;
  preview.upgrade.clear();
  REQUIRE_FALSE(tui::preview_mark_issues(preview, marks, {}).front().empty());
  REQUIRE(tui::preview_mark_issues(preview, marks, { marks.front().action.nevra }).front().empty());
  REQUIRE_FALSE(tui::preview_mark_issues(preview, marks, { "demo-3-1.noarch" }).front().empty());

  preview.install.clear();
  preview.remove.push_back({ "demo-2-1.noarch", "demo", "noarch" });
  REQUIRE_FALSE(tui::preview_mark_issues(preview, marks, {}).front().empty());
}

TEST_CASE("TUI transaction preview requires an exact marked removal")
{
  tui::PendingMark mark {};
  mark.action = { PendingAction::REMOVE, "demo-1-1.noarch", "demo-1-1.noarch", "demo\nnoarch" };
  TransactionPreview preview;
  preview.remove.push_back({ "demo-2-1.noarch", "demo", "noarch" });
  REQUIRE_FALSE(tui::preview_mark_issues(preview, { mark }, {}).front().empty());
  preview.remove.front().label = mark.action.nevra;
  REQUIRE(tui::preview_mark_issues(preview, { mark }, {}).front().empty());
}

TEST_CASE("TUI transaction preview describes warnings and additional removals")
{
  TransactionPreview preview;
  preview.requires_offline = true;
  preview.resolve_warnings = "Resolve warning";
  preview.remove.push_back({ "dependency-1-1.noarch", "dependency", "noarch", true });
  preview.replaced.push_back({ "demo-1-1.noarch", "demo", "noarch" });
  preview.disk_space_delta = -1000;
  const auto text = tui::format_preview(preview);
  REQUIRE(text.find("next reboot") != std::string::npos);
  REQUIRE(text.find("Resolve warning") != std::string::npos);
  REQUIRE(text.find("dependency-1-1.noarch [additional removal]") != std::string::npos);
  REQUIRE(text.find("Replace (1)") != std::string::npos);
  REQUIRE(text.find("freed") != std::string::npos);
}

TEST_CASE("TUI preview reports each mismatched mark without flagging matching actions")
{
  auto upgrade = upgrade_mark();
  tui::PendingMark removal {};
  removal.action = { PendingAction::REMOVE, "other-1-1.noarch", "other-1-1.noarch", "other\nnoarch" };
  tui::PendingMark install {};
  install.action = { PendingAction::INSTALL, "new-1-1.noarch", "new-1-1.noarch", "new\nnoarch" };

  TransactionPreview preview;
  preview.remove.push_back({ removal.action.nevra, "other", "noarch" });
  const std::vector<tui::PendingMark> marks { upgrade, removal, install };
  const auto issues = tui::preview_mark_issues(preview, marks, {});
  REQUIRE(issues.size() == marks.size());
  REQUIRE(issues[0].find(upgrade.action.nevra) != std::string::npos);
  REQUIRE(issues[1].empty());
  REQUIRE(issues[2].find(install.action.nevra) != std::string::npos);

  const auto empty_issues = tui::preview_mark_issues({}, marks, {});
  for (const auto &issue : empty_issues) {
    REQUIRE_FALSE(issue.empty());
  }
}
