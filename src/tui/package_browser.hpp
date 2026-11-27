// -----------------------------------------------------------------------------
// src/tui/package_browser.hpp
// -----------------------------------------------------------------------------
#pragma once

#include "package_query.hpp"
#include "pending_actions.hpp"
#include "shutdown.hpp"
#include "transaction_preview.hpp"

#include <atomic>
#include <cstddef>
#include <cwchar>
#include <future>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <gio/gio.h>

namespace tui {

class PackageBrowser {
  public:
  ~PackageBrowser();
  void cancel();
  void run(bool colors_enabled, const tui::ShutdownSignals &shutdown);

  private:
  static constexpr int MIN_HEIGHT = 8;
  static constexpr int MIN_WIDTH = 50;

  enum class Screen {
    PACKAGES,
    SEARCH,
    PACKAGE_DETAILS,
    ACTION_MENU,
    PENDING,
    PENDING_DETAILS,
    PREVIEW,
    CONFIRM_APPLY,
    APPLYING,
    APPLY_RESULT,
  };

  enum class WorkerOperation {
    NONE,
    PACKAGE_QUERY,
    PACKAGE_DETAILS,
    PENDING_CHECK,
    PREVIEW,
    RELEASE_PREVIEW,
    APPLY,
  };

  struct PackageQueryResult {
    QueryResult query;
    tui::OfflineState offline_state;
  };

  using WorkerResult = std::variant<QueryResult, PackageQueryResult, tui::PreviewResult, tui::ApplyResult>;

  // The worker owns package queries. Only the terminal thread reads UI state
  // and draws to ncurses.
  std::unique_ptr<GCancellable, decltype(&g_object_unref)> cancellable { g_cancellable_new(), g_object_unref };
  // BaseManager rebuilds use an atomic cancellation flag rather than GCancellable.
  std::shared_ptr<std::atomic<bool>> cancel_requested = std::make_shared<std::atomic<bool>>(false);
  std::future<WorkerResult> worker;
  std::vector<DisplayRow> rows;
  size_t selected = 0;
  size_t first = 0;
  size_t details_first = 0;
  std::string details;
  std::string notice;
  std::string list_notice;
  std::string term;
  std::string active_search;
  PackageView active_view = PackageView::INSTALLED;
  bool colors = false;
  bool has_queried = false;
  Screen screen = Screen::PACKAGES;
  WorkerOperation operation = WorkerOperation::NONE;
  std::vector<tui::PendingMark> pending;
  bool reload_after_pending = false;
  std::unique_ptr<tui::PreparedTransaction> prepared;
  std::shared_ptr<tui::TransactionProgress> apply_progress;
  tui::OfflineState offline_state = tui::OfflineState::UNKNOWN;
  size_t pending_selected = 0;
  size_t pending_first = 0;

  void query(PackageView view, const std::string &term = {}, bool refresh_metadata = false);
  void update_pending_issues(std::vector<std::string> issues);
  bool collect_result();
  void load_details();
  void handle_search_key(int kind, wint_t key);
  void handle_list_key(int kind, wint_t key);
  void handle_action_key(int kind, wint_t key);
  void handle_pending_key(int kind, wint_t key);
  void handle_preview_key(int kind, wint_t key);
  void start_apply();
  void finish_apply(const tui::ApplyResult &result);
  void handle_details_key(int kind, wint_t key);

  static int list_page_rows(int height);
  static const char *row_status(const DisplayRow &row);

  void draw();
  void draw_action_menu(int height, int width);
  void draw_pending(int height, int width);
  void draw_search(int width);
  void draw_summary(int y, int width);
  void draw_list(int height, int width);
};

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
