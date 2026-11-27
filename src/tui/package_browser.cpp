// -----------------------------------------------------------------------------
// src/tui/package_browser.cpp
// -----------------------------------------------------------------------------
#include "package_browser.hpp"

#include "dnf_backend/base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "terminal.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <utility>

namespace tui {

PackageBrowser::~PackageBrowser()
{
  if (worker.valid()) {
    cancel();
    worker.wait();
  }
}

void
PackageBrowser::cancel()
{
  if (operation == WorkerOperation::APPLY) {
    return;
  }
  cancel_requested->store(true, std::memory_order_relaxed);
  g_cancellable_cancel(cancellable.get());
}

void
PackageBrowser::query(PackageView view, const std::string &term, bool refresh_metadata)
{
  active_view = view;
  active_search = view == PackageView::SEARCH ? term : "";
  rows.clear();
  selected = 0;
  first = 0;
  details.clear();
  screen = Screen::PACKAGES;
  notice = view == PackageView::SEARCH ? "Searching packages..." : "Loading packages...";
  list_notice.clear();
  operation = WorkerOperation::PACKAGE_QUERY;
  if (refresh_metadata) {
    notice = "Refreshing repository metadata...";
  } else if (view == PackageView::UPGRADABLE) {
    notice = "Loading upgradable packages from dnf5daemon...";
  }

  const bool refresh_snapshot = has_queried;
  const bool check_offline = !has_queried || refresh_metadata;
  has_queried = true;
  if (check_offline) {
    offline_state = tui::OfflineState::UNKNOWN;
  }
  worker = std::async(std::launch::async,
                      [view,
                       term,
                       refresh_snapshot,
                       refresh_metadata,
                       check_offline,
                       previous_offline = offline_state,
                       cancel = cancellable.get(),
                       cancel_flag = cancel_requested]() -> WorkerResult {
                        const auto offline = check_offline ? tui::query_offline_state(cancel) : previous_offline;
                        return PackageQueryResult {
                          query_packages(view, term, refresh_snapshot, refresh_metadata, cancel, cancel_flag), offline
                        };
                      });
}

void
PackageBrowser::run(bool colors_enabled, const tui::ShutdownSignals &shutdown)
{
  colors = colors_enabled;
  if (shutdown.requested()) {
    return;
  }
  query(PackageView::INSTALLED);
  bool redraw = true;
  while (!shutdown.requested()) {
    if (collect_result()) {
      redraw = true;
    }
    if (operation == WorkerOperation::APPLY && apply_progress) {
      const auto progress = apply_progress->text();
      if (!progress.empty() && progress != notice) {
        notice = progress;
        redraw = true;
      }
    }
    if (redraw) {
      draw();
      redraw = false;
    }

    wint_t key = 0;
    const int kind = wget_wch(stdscr, &key);
    if (shutdown.requested()) {
      return;
    }
    if (kind == ERR) {
      if (terminal_input_closed()) {
        return;
      }
      continue;
    }
    if (kind == OK && key == 3) {
      if (operation == WorkerOperation::APPLY) {
        continue;
      }
      return;
    }
    if ((kind == KEY_CODE_YES && key == KEY_RESIZE) || (kind == OK && key == 12)) {
      clearok(stdscr, true);
      redraw = true;
      continue;
    }
    if (screen == Screen::SEARCH) {
      handle_search_key(kind, key);
      redraw = true;
      continue;
    }
    if (kind == OK && (key == L'q' || key == L'Q')) {
      if (operation == WorkerOperation::APPLY) {
        continue;
      }
      return;
    }
    if (worker.valid()) {
      continue;
    }
    redraw = true;
    switch (screen) {
    case Screen::PACKAGE_DETAILS:
    case Screen::PENDING_DETAILS:
      handle_details_key(kind, key);
      break;
    case Screen::PREVIEW:
    case Screen::CONFIRM_APPLY:
      handle_preview_key(kind, key);
      break;
    case Screen::APPLY_RESULT:
      if (kind == OK && key == 27) {
        query(active_view, active_search);
      } else {
        handle_details_key(kind, key);
      }
      break;
    case Screen::APPLYING:
      break;
    case Screen::ACTION_MENU:
      handle_action_key(kind, key);
      break;
    case Screen::PENDING:
      handle_pending_key(kind, key);
      break;
    case Screen::PACKAGES:
      handle_list_key(kind, key);
      break;
    case Screen::SEARCH:
      break;
    }
  }
}

void
PackageBrowser::update_pending_issues(std::vector<std::string> issues)
{
  if (issues.size() == pending.size()) {
    for (size_t i = 0; i < pending.size(); ++i) {
      pending[i].issue = std::move(issues[i]);
    }
  } else {
    for (auto &mark : pending) {
      mark.issue = "Unchecked: validation did not finish.";
    }
  }
}

bool
PackageBrowser::collect_result()
{
  if (!worker.valid() || worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    return false;
  }
  const auto completed = operation;
  operation = WorkerOperation::NONE;
  try {
    auto value = worker.get();
    if (completed == WorkerOperation::PREVIEW) {
      auto &result = std::get<tui::PreviewResult>(value);
      update_pending_issues(std::move(result.pending_issues));
      tui::update_pending_preview_issues(pending, std::move(result.preview_issues));
      offline_state = result.offline_state;
      prepared = std::move(result.prepared);
      details = result.error.empty() ? tui::format_preview(result.preview) : std::move(result.error);
      details_first = 0;
      notice = prepared ? "Transaction preview" : "Transaction preview failed";
      screen = Screen::PREVIEW;
      return true;
    }
    if (completed == WorkerOperation::APPLY) {
      auto &result = std::get<tui::ApplyResult>(value);
      finish_apply(result);
      return true;
    }
    if (completed == WorkerOperation::RELEASE_PREVIEW) {
      notice.clear();
      return true;
    }
    if (completed == WorkerOperation::PACKAGE_QUERY) {
      offline_state = std::get<PackageQueryResult>(value).offline_state;
    }
    auto &result = completed == WorkerOperation::PACKAGE_QUERY ? std::get<PackageQueryResult>(value).query
                                                               : std::get<QueryResult>(value);
    if (completed == WorkerOperation::PENDING_CHECK) {
      update_pending_issues(std::move(result.pending_issues));
      screen = Screen::PENDING;
    } else if (completed == WorkerOperation::PACKAGE_DETAILS) {
      details = std::move(result.details);
      details_first = 0;
      screen = Screen::PACKAGE_DETAILS;
      notice.clear();
    } else {
      rows = std::move(result.rows);
      list_notice = std::move(result.notice);
      notice = list_notice;
      if (!pending.empty()) {
        const auto snapshot = DaemonUpgradeState::instance().snapshot();
        for (auto &mark : pending) {
          const std::string issue = tui::pending_snapshot_issue(mark, snapshot);
          if (!issue.empty() || mark.action.type == PendingAction::REMOVE ||
              (mark.action.type == PendingAction::UPGRADE && snapshot.status == DaemonUpgradeSnapshotStatus::READY)) {
            mark.issue = issue;
          }
        }
      }
    }
  } catch (const std::exception &error) {
    if (completed == WorkerOperation::APPLY) {
      DaemonUpgradeState::instance().mark_stale();
      BaseManager::instance().drop_cached_base();
      tui::ApplyResult failed;
      failed.message = std::string(error.what()) +
          "\n\nInstalled packages may have changed. Review package state and dnf5 offline status before retrying.";
      finish_apply(failed);
      return true;
    }
    if (completed == WorkerOperation::PREVIEW) {
      prepared.reset();
      for (auto &mark : pending) {
        mark.issue = "Unchecked: preview failed.";
      }
      details = error.what();
      details_first = 0;
      notice = "Transaction preview failed";
      screen = Screen::PREVIEW;
      return true;
    } else if (completed == WorkerOperation::PENDING_CHECK) {
      screen = Screen::PENDING;
      for (auto &mark : pending) {
        mark.issue = "Unchecked: validation failed.";
      }
      std::cerr << "Pending validation failed: " << error.what() << '\n';
    } else if (completed == WorkerOperation::PACKAGE_QUERY) {
      for (auto &mark : pending) {
        mark.issue = "Unchecked: package query failed.";
      }
    }
    notice = error.what();
  }
  return true;
}

void
PackageBrowser::load_details()
{
  const auto &row = rows[selected];
  if (row.upgrade) {
    // These details come from the same daemon result as the selected row.
    details = std::string("Status: ") + row_status(row) + "\nPackage ID: " + row.package.nevra +
        "\nRepo: " + row.package.repo + "\n\nSummary: " + row.package.summary +
        "\n\nUpgrade target reported by dnf5daemon. This is not a transaction preview.";
    details_first = 0;
    screen = Screen::PACKAGE_DETAILS;
    return;
  }

  const std::string nevra = row.package.nevra;
  const std::string status = row_status(row);
  const auto context =
      row.installed ? PackageDetailsContext::INSTALLED_CONTEXT : PackageDetailsContext::SELECTED_VERSION;
  operation = WorkerOperation::PACKAGE_DETAILS;
  notice = "Loading package details...";
  worker = std::async(std::launch::async, [nevra, status, context, cancel = cancellable.get()]() -> WorkerResult {
    QueryResult result;
    if (g_cancellable_is_cancelled(cancel)) {
      return result;
    }

    // The backend detail lookup cannot be interrupted once it starts.
    const auto info = dnf_backend_get_package_info_presentation(nevra, context);
    if (g_cancellable_is_cancelled(cancel)) {
      return result;
    }
    result.details = "Status: " + status + "\n" + nevra + "\n\n" + info.full_text;
    return result;
  });
}

void
PackageBrowser::handle_search_key(int kind, wint_t key)
{
  if (kind == OK && key == 27) {
    screen = Screen::PACKAGES;
  } else if ((kind == OK && (key == L'\n' || key == L'\r')) || (kind == KEY_CODE_YES && key == KEY_ENTER)) {
    if (!term.empty()) {
      query(PackageView::SEARCH, term);
    }
  } else if ((kind == KEY_CODE_YES && key == KEY_BACKSPACE) || (kind == OK && (key == 127 || key == 8))) {
    if (!term.empty()) {
      const char *last = g_utf8_find_prev_char(term.c_str(), term.c_str() + term.size());
      if (last) {
        term.resize(static_cast<size_t>(last - term.c_str()));
      }
    }
  } else if (kind == OK && g_unichar_isprint(static_cast<gunichar>(key))) {
    char encoded[6];
    const int length = g_unichar_to_utf8(static_cast<gunichar>(key), encoded);
    term.append(encoded, length);
  }
}

void
PackageBrowser::handle_list_key(int kind, wint_t key)
{
  if (kind == OK && key == L'/') {
    term.clear();
    screen = Screen::SEARCH;
  } else if (kind == OK && key == L'i') {
    query(PackageView::INSTALLED);
  } else if (kind == OK && key == L'a') {
    query(PackageView::ALL);
  } else if (kind == OK && key == L'u') {
    query(PackageView::UNNEEDED);
  } else if (kind == OK && key == L'g') {
    query(PackageView::UPGRADABLE);
  } else if (kind == OK && key == L'r') {
    query(active_view, active_search, true);
  } else if (kind == OK && key == L'p') {
    pending_selected = 0;
    pending_first = 0;
    reload_after_pending = !pending.empty();
    if (pending.empty()) {
      screen = Screen::PENDING;
    } else {
      operation = WorkerOperation::PENDING_CHECK;
      notice = "Checking pending actions...";
      worker =
          std::async(std::launch::async,
                     [marks = pending, cancel = cancellable.get(), cancel_flag = cancel_requested]() -> WorkerResult {
                       QueryResult result;
                       result.pending_issues = tui::validate_pending_marks(marks, cancel, cancel_flag);
                       return result;
                     });
    }
  } else if (!rows.empty()) {
    if (kind == OK && key == L'm') {
      screen = Screen::ACTION_MENU;
      return;
    }
    const size_t page = static_cast<size_t>(list_page_rows(getmaxy(stdscr)));
    if (kind == KEY_CODE_YES && key == KEY_UP && selected > 0) {
      --selected;
    } else if (kind == KEY_CODE_YES && key == KEY_DOWN && selected + 1 < rows.size()) {
      ++selected;
    } else if (kind == KEY_CODE_YES && key == KEY_PPAGE) {
      selected = selected > page ? selected - page : 0;
    } else if (kind == KEY_CODE_YES && key == KEY_NPAGE) {
      selected = std::min(selected + page, rows.size() - 1);
    } else if (kind == KEY_CODE_YES && key == KEY_HOME) {
      selected = 0;
    } else if (kind == KEY_CODE_YES && key == KEY_END) {
      selected = rows.size() - 1;
    } else if ((kind == OK && (key == L'\n' || key == L'\r')) || (kind == KEY_CODE_YES && key == KEY_ENTER)) {
      load_details();
    }
  }
}

void
PackageBrowser::handle_action_key(int kind, wint_t key)
{
  if (kind != OK) {
    return;
  }
  if (key == 27) {
    screen = Screen::PACKAGES;
    return;
  }

  const auto &row = rows[selected];
  if (key == L'x') {
    const auto action = std::find_if(pending.begin(), pending.end(), [&](const tui::PendingMark &item) {
      return item.action.nevra == row.package.nevra;
    });
    if (action != pending.end()) {
      pending.erase(action);
      notice = "Unmarked: " + row.package.name;
      screen = Screen::PACKAGES;
    }
    return;
  }

  const auto choices = tui::pending_choices(row);
  PendingAction::Type type;
  if (key == L'1' && choices.can_install) {
    type = tui::install_side_type(choices.rows);
  } else if (key == L'2' && choices.can_remove) {
    type = PendingAction::REMOVE;
  } else if (key == L'3' && choices.can_reinstall) {
    type = PendingAction::REINSTALL;
  } else {
    return;
  }

  if (tui::mark_pending_action(pending, row, type)) {
    notice = std::string("Marked for ") + tui::pending_action_name(type) + ": " + row.package.name;
  } else {
    notice = "Package action is no longer available.";
  }
  screen = Screen::PACKAGES;
}

void
PackageBrowser::handle_pending_key(int kind, wint_t key)
{
  if (kind == OK && (key == 27 || key == L'p')) {
    screen = Screen::PACKAGES;
    if (reload_after_pending) {
      reload_after_pending = false;
      // Validation may have replaced the installed or daemon snapshot.
      // Reload the package list before accepting any new marks.
      query(active_view, active_search);
    } else {
      notice = list_notice;
    }
    return;
  }
  if (pending.empty()) {
    return;
  }

  if (kind == OK && key == L'v') {
    operation = WorkerOperation::PREVIEW;
    reload_after_pending = true;
    notice = "Preparing transaction preview...";
    worker =
        std::async(std::launch::async,
                   [marks = pending, cancel = cancellable.get(), cancel_flag = cancel_requested]() -> WorkerResult {
                     auto result = tui::preview_pending_marks(marks, cancel, cancel_flag);
                     if (result.prepared) {
                       // The daemon checked that no offline transaction was stored before resolving.
                       result.offline_state = tui::OfflineState::NOT_PREPARED;
                     } else {
                       result.offline_state = tui::query_offline_state(cancel);
                       if (!result.error.empty() && result.offline_state == tui::OfflineState::PREPARED) {
                         std::cerr << "Transaction preview failed: " << result.error << '\n';
                         result.error =
                             std::string("Updates are already prepared for reboot.\n\n") + tui::offline_recovery_hint();
                       }
                     }
                     return result;
                   });
  } else if (kind == OK && key == L'x') {
    pending.erase(pending.begin() + pending_selected);
    if (pending_selected >= pending.size() && pending_selected > 0) {
      --pending_selected;
    }
  } else if (kind == OK && key == L'c') {
    pending.clear();
    pending_selected = 0;
    pending_first = 0;
  } else if (kind == KEY_CODE_YES && key == KEY_UP && pending_selected > 0) {
    --pending_selected;
  } else if (kind == KEY_CODE_YES && key == KEY_DOWN && pending_selected + 1 < pending.size()) {
    ++pending_selected;
  } else if (kind == KEY_CODE_YES && key == KEY_HOME) {
    pending_selected = 0;
  } else if (kind == KEY_CODE_YES && key == KEY_END) {
    pending_selected = pending.size() - 1;
  } else if ((kind == OK && (key == L'\n' || key == L'\r')) || (kind == KEY_CODE_YES && key == KEY_ENTER)) {
    const auto &mark = pending[pending_selected];
    const auto &action = mark.action;
    details = std::string("Pending action: ") + tui::pending_action_name(action.type) + "\n\nPackage: " + action.nevra +
        "\n\nRequest spec: " + action.transaction_spec +
        "\n\nCheck: " + (mark.has_issue() ? mark.issue_text() : "No known issue");
    details_first = 0;
    screen = Screen::PENDING_DETAILS;
  } else if (kind == KEY_CODE_YES && key == KEY_PPAGE) {
    const size_t page = static_cast<size_t>(std::max(1, getmaxy(stdscr) - 5));
    pending_selected = pending_selected > page ? pending_selected - page : 0;
  } else if (kind == KEY_CODE_YES && key == KEY_NPAGE) {
    const size_t page = static_cast<size_t>(std::max(1, getmaxy(stdscr) - 5));
    pending_selected = std::min(pending_selected + page, pending.size() - 1);
  }
}

void
PackageBrowser::handle_preview_key(int kind, wint_t key)
{
  if (kind == OK && key == 27 && screen == Screen::PREVIEW) {
    screen = Screen::PENDING;
    if (prepared) {
      notice = "Releasing transaction preview...";
      operation = WorkerOperation::RELEASE_PREVIEW;
      worker = std::async(std::launch::async, [session = std::move(prepared)]() mutable -> WorkerResult {
        session.reset();
        return QueryResult {};
      });
    } else {
      notice.clear();
    }
    return;
  }
  if (screen == Screen::CONFIRM_APPLY) {
    if (kind == OK && (key == 27 || key == L'n')) {
      screen = Screen::PREVIEW;
      notice = "Transaction preview";
      return;
    }
    if (kind == OK && key == L'y' && prepared && getmaxy(stdscr) >= MIN_HEIGHT && getmaxx(stdscr) >= MIN_WIDTH) {
      start_apply();
      return;
    }
  } else if (kind == OK && key == L'a' && prepared) {
    screen = Screen::CONFIRM_APPLY;
    notice = prepared->requires_offline() ? "Prepare these changes for reboot?" : "Apply these package changes?";
    return;
  }
  if (kind != OK || key != 27) {
    handle_details_key(kind, key);
  }
}

void
PackageBrowser::start_apply()
{
  screen = Screen::APPLYING;
  operation = WorkerOperation::APPLY;
  notice = "Submitting approved transaction...";
  apply_progress = std::make_shared<tui::TransactionProgress>();
  worker = std::async(std::launch::async,
                      [session = std::move(prepared), progress = apply_progress]() mutable -> WorkerResult {
                        return tui::apply_transaction(std::move(session), *progress);
                      });
}

void
PackageBrowser::finish_apply(const tui::ApplyResult &result)
{
  apply_progress.reset();
  offline_state = result.offline_state;
  rows.clear();
  selected = 0;
  first = 0;
  // Keep marks only when offline preparation failed, as in the desktop frontend.
  if (result.success || !result.offline) {
    pending.clear();
  } else {
    for (auto &mark : pending) {
      mark.issue = "Unchecked: offline preparation failed.";
    }
  }
  pending_selected = 0;
  pending_first = 0;
  reload_after_pending = false;
  details = result.message;
  details_first = 0;
  notice = result.success ? (result.offline ? "Prepared for reboot" : "Transaction successful") : "Transaction failed";
  screen = Screen::APPLY_RESULT;
}

void
PackageBrowser::handle_details_key(int kind, wint_t key)
{
  if (kind == OK && key == 27) {
    screen = screen == Screen::PENDING_DETAILS || screen == Screen::PREVIEW ? Screen::PENDING : Screen::PACKAGES;
    notice = list_notice;
  } else if (kind == KEY_CODE_YES && key == KEY_UP && details_first > 0) {
    --details_first;
  } else if (kind == KEY_CODE_YES && key == KEY_DOWN) {
    ++details_first;
  } else if (kind == KEY_CODE_YES && key == KEY_PPAGE) {
    const size_t page = static_cast<size_t>(std::max(1, getmaxy(stdscr) - 4));
    details_first = details_first > page ? details_first - page : 0;
  } else if (kind == KEY_CODE_YES && key == KEY_NPAGE) {
    details_first += static_cast<size_t>(std::max(1, getmaxy(stdscr) - 4));
  }
}

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
