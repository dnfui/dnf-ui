// -----------------------------------------------------------------------------
// src/tui/main.cpp
// Read-only terminal frontend for the DNF backend
// -----------------------------------------------------------------------------
#include "dnf_backend/base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "terminal.hpp"
#include "upgrade_query.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using tui::DisplayRow;
using tui::draw_bar;
using tui::draw_text;
using tui::HEADING_COLOR;
using tui::INSTALLED_COLOR;
using tui::QueryResult;
using tui::terminal_input_closed;
using tui::wide_text;
using tui::wrap_text;

constexpr int MIN_HEIGHT = 8;
constexpr int MIN_WIDTH = 50;
constexpr int SUMMARY_MIN_HEIGHT = 12;
constexpr int SUMMARY_HEIGHT = 3;
constexpr int TABLE_FIRST_ROW = 4;
constexpr int FOOTER_HEIGHT = 2;

// Reserve room for the summary only when there is enough space for the table.
int
list_page_rows(int height)
{
  const int summary = height >= SUMMARY_MIN_HEIGHT ? SUMMARY_HEIGHT : 0;
  return std::max(1, height - TABLE_FIRST_ROW - FOOTER_HEIGHT - summary);
}

enum class PackageView {
  INSTALLED,
  ALL,
  UNNEEDED,
  UPGRADABLE,
  SEARCH,
};

const char *
view_name(PackageView view)
{
  switch (view) {
  case PackageView::INSTALLED:
    return "Installed packages";
  case PackageView::ALL:
    return "All packages";
  case PackageView::UNNEEDED:
    return "Unneeded packages";
  case PackageView::UPGRADABLE:
    return "Upgradable packages";
  case PackageView::SEARCH:
    return "Search results";
  }
  return "Unknown";
}

const char *
row_status(const DisplayRow &row)
{
  if (row.upgrade) {
    return "Upgradable";
  }
  return row.installed ? "Installed" : "Available";
}

class PackageBrowser {
  public:
  ~PackageBrowser()
  {
    if (worker.valid()) {
      cancel();
      worker.wait();
    }
  }

  void cancel()
  {
    cancel_requested->store(true, std::memory_order_relaxed);
    g_cancellable_cancel(cancellable.get());
  }

  void query(PackageView view, const std::string &term = {}, bool refresh_metadata = false)
  {
    active_view = view;
    active_search = view == PackageView::SEARCH ? term : "";
    rows.clear();
    selected = 0;
    first = 0;
    details.clear();
    showing_details = false;
    notice = view == PackageView::SEARCH ? "Searching packages..." : "Loading packages...";
    list_notice.clear();
    loading_details = false;
    if (refresh_metadata) {
      notice = "Refreshing repository metadata...";
    } else if (view == PackageView::UPGRADABLE) {
      notice = "Loading upgradable packages from dnf5daemon...";
    }

    const bool refresh_snapshot = has_queried;
    has_queried = true;
    worker = std::async(
        std::launch::async,
        [view, term, refresh_snapshot, refresh_metadata, cancel = cancellable.get(), cancel_flag = cancel_requested] {
          QueryResult result;
          if (g_cancellable_is_cancelled(cancel)) {
            return result;
          }

          if (view == PackageView::UPGRADABLE) {
            return tui::query_upgrades(refresh_metadata, cancel, cancel_flag);
          }
          DaemonUpgradeState::instance().mark_stale();

          // A new Base already reads the rpmdb. On later queries, check for
          // external changes without discarding usable repository metadata.
          auto &manager = BaseManager::instance();
          std::string daemon_refresh_error;
          if (refresh_metadata) {
            daemon_refresh_error = tui::refresh_daemon_metadata(cancel);
            if (g_cancellable_is_cancelled(cancel)) {
              return result;
            }
            manager.rebuild(BaseRefreshMode::FORCE_METADATA_CHECK, cancel_flag);
          } else if (view == PackageView::UNNEEDED) {
            // Installation reasons can change without changing installed NEVRAs.
            // Reload system state using normal repository expiration rules.
            manager.rebuild(BaseRefreshMode::NORMAL, cancel_flag);
          } else if (refresh_snapshot) {
            const bool changed = dnf_backend_refresh_installed_snapshot_preserving_cached_base();
            if (g_cancellable_is_cancelled(cancel)) {
              return result;
            }
            if (changed || manager.current_repo_state() != BaseRepoState::LIVE_METADATA) {
              manager.drop_cached_base();
            }
          }

          if (g_cancellable_is_cancelled(cancel)) {
            return result;
          }
          std::vector<PackageRow> packages;
          switch (view) {
          case PackageView::INSTALLED:
            packages = dnf_backend_get_installed_package_rows_interruptible(cancel);
            break;
          case PackageView::ALL:
            packages = dnf_backend_get_browse_package_rows_interruptible({}, cancel);
            break;
          case PackageView::UNNEEDED:
            packages = dnf_backend_get_unneeded_package_rows_interruptible(cancel);
            break;
          case PackageView::SEARCH:
            packages = dnf_backend_search_package_rows_interruptible(term, {}, cancel);
            break;
          case PackageView::UPGRADABLE:
            break;
          }
          if (g_cancellable_is_cancelled(cancel)) {
            return result;
          }
          const BaseRepoState repo_state = manager.current_repo_state();
          std::sort(packages.begin(), packages.end(), [](const PackageRow &a, const PackageRow &b) {
            return a.nevra < b.nevra;
          });

          result.rows.reserve(packages.size());
          for (auto &package : packages) {
            if (g_cancellable_is_cancelled(cancel)) {
              return result;
            }
            const bool exact_installed = dnf_backend_resolve_installed_package(package).exact_installed;
            result.rows.push_back({ std::move(package), exact_installed });
          }
          result.notice = std::to_string(result.rows.size()) + " packages";
          if (repo_state == BaseRepoState::INSTALLED_ONLY) {
            result.notice += " (repository metadata unavailable)";
          } else if (repo_state == BaseRepoState::CACHED_METADATA) {
            result.notice += " (cached repository metadata)";
          }
          if (!daemon_refresh_error.empty()) {
            std::cerr << "dnf5daemon refresh failed: " << daemon_refresh_error << '\n';
            result.notice = "Daemon refresh failed; " + result.notice;
          }
          return result;
        });
  }

  void run(bool colors_enabled)
  {
    colors = colors_enabled;
    query(PackageView::INSTALLED);
    bool redraw = true;
    while (true) {
      if (collect_result()) {
        redraw = true;
      }
      if (redraw) {
        draw();
        redraw = false;
      }

      wint_t key = 0;
      const int kind = wget_wch(stdscr, &key);
      if (kind == ERR) {
        if (terminal_input_closed()) {
          return;
        }
        continue;
      }
      if (kind == OK && key == 3) {
        return;
      }
      if ((kind == KEY_CODE_YES && key == KEY_RESIZE) || (kind == OK && key == 12)) {
        clearok(stdscr, true);
        redraw = true;
        continue;
      }
      if (editing) {
        handle_search_key(kind, key);
        redraw = true;
        continue;
      }
      if (kind == OK && (key == L'q' || key == L'Q')) {
        return;
      }
      if (worker.valid()) {
        continue;
      }
      redraw = true;
      if (showing_details) {
        handle_details_key(kind, key);
      } else {
        handle_list_key(kind, key);
      }
    }
  }

  private:
  // The worker owns all backend calls. Only the terminal thread reads UI state
  // and draws to ncurses.
  std::unique_ptr<GCancellable, decltype(&g_object_unref)> cancellable { g_cancellable_new(), g_object_unref };
  // BaseManager rebuilds use an atomic cancellation flag rather than GCancellable.
  std::shared_ptr<std::atomic<bool>> cancel_requested = std::make_shared<std::atomic<bool>>(false);
  std::future<QueryResult> worker;
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
  bool editing = false;
  bool loading_details = false;
  bool showing_details = false;

  bool collect_result()
  {
    if (!worker.valid() || worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      return false;
    }
    try {
      auto result = worker.get();
      if (loading_details) {
        details = std::move(result.details);
        details_first = 0;
        showing_details = true;
        notice.clear();
      } else {
        rows = std::move(result.rows);
        list_notice = std::move(result.notice);
        notice = list_notice;
      }
    } catch (const std::exception &error) {
      notice = error.what();
    }
    return true;
  }

  void load_details()
  {
    const auto &row = rows[selected];
    if (row.upgrade) {
      // These details come from the same daemon result as the selected row.
      details = std::string("Status: ") + row_status(row) + "\nPackage ID: " + row.package.nevra +
          "\nRepo: " + row.package.repo + "\n\nSummary: " + row.package.summary +
          "\n\nUpgrade target reported by dnf5daemon. This is not a transaction preview.";
      details_first = 0;
      showing_details = true;
      return;
    }

    const std::string nevra = row.package.nevra;
    const std::string status = row_status(row);
    const auto context =
        row.installed ? PackageDetailsContext::INSTALLED_CONTEXT : PackageDetailsContext::SELECTED_VERSION;
    loading_details = true;
    notice = "Loading package details...";
    worker = std::async(std::launch::async, [nevra, status, context, cancel = cancellable.get()] {
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

  void handle_search_key(int kind, wint_t key)
  {
    if (kind == OK && key == 27) {
      editing = false;
    } else if ((kind == OK && (key == L'\n' || key == L'\r')) || (kind == KEY_CODE_YES && key == KEY_ENTER)) {
      if (!term.empty()) {
        editing = false;
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

  void handle_list_key(int kind, wint_t key)
  {
    if (kind == OK && key == L'/') {
      term.clear();
      editing = true;
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
    } else if (!rows.empty()) {
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

  void handle_details_key(int kind, wint_t key)
  {
    if (kind == OK && key == 27) {
      showing_details = false;
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

  void draw()
  {
    const int height = getmaxy(stdscr);
    const int width = getmaxx(stdscr);
    erase();
    if (height < MIN_HEIGHT || width < MIN_WIDTH) {
      draw_text(0, 0, width - 1, "Terminal too small (50 x 8 required)");
      curs_set(0);
      refresh();
      return;
    }

    draw_bar(0, width, " DNF UI - Terminal (read-only)");

    if (showing_details) {
      const auto lines = wrap_text(details, width);
      const size_t page = static_cast<size_t>(height - 4);
      const size_t last = lines.size() > page ? lines.size() - page : 0;
      details_first = std::min(details_first, last);
      for (size_t i = 0; i < page && details_first + i < lines.size(); ++i) {
        draw_text(static_cast<int>(i) + 2, 0, width, lines[details_first + i]);
      }
      draw_bar(height - 1, width, " Up/Down: Scroll  Esc: Back  q: Quit");
    } else {
      draw_list(height, width);
    }
    if (editing) {
      curs_set(1);
    } else {
      curs_set(0);
    }
    refresh();
  }

  // Show the applied search separately from the editable input so cancelling
  // an edit does not change the query displayed above the package table.
  void draw_search(int width)
  {
    attron(A_BOLD);
    draw_text(1, 0, width, "Search: ");
    attroff(A_BOLD);

    if (!editing) {
      draw_text(1, 8, width - 8, active_search.empty() ? "(press / to search)" : active_search);
      return;
    }

    const auto input = wide_text(term);
    int cells = 0;
    size_t start = input.size();
    while (start > 0) {
      const int next = wcwidth(input[start - 1]);
      if (cells + next > width - 9) {
        break;
      }
      cells += next;
      --start;
    }
    attron(A_UNDERLINE);
    draw_text(1, 8, width - 8, input.substr(start));
    attroff(A_UNDERLINE);
    wmove(stdscr, 1, 8 + cells);
  }

  void draw_summary(int y, int width)
  {
    mvhline(y, 0, ACS_HLINE, width);
    if (rows.empty()) {
      draw_text(y + 1, 0, width, "No package selected");
      return;
    }

    const auto &package = rows[selected].package;
    attron(A_BOLD);
    draw_text(y + 1, 0, width, package.nevra);
    attroff(A_BOLD);
    draw_text(y + 2, 0, width, package.summary);
  }

  void draw_list(int height, int width)
  {
    const int status_width = 11;
    const int arch_width = 10;
    const int name_width = (width - status_width - arch_width) / 2;
    const int name_x = status_width;
    const int version_x = name_x + name_width;
    const int arch_x = width - arch_width;
    const int version_width = arch_x - version_x;
    const size_t page = static_cast<size_t>(list_page_rows(height));

    if (selected < first) {
      first = selected;
    } else if (selected - first >= page) {
      first = selected - page + 1;
    }
    const size_t last_first = rows.size() > page ? rows.size() - page : 0;
    first = std::min(first, last_first);

    attron(A_BOLD);
    if (colors) {
      attron(COLOR_PAIR(HEADING_COLOR));
    }
    draw_text(2, 0, width, std::string("View: ") + view_name(active_view));
    if (colors) {
      attroff(COLOR_PAIR(HEADING_COLOR));
    }
    attroff(A_BOLD);

    attron(A_REVERSE);
    mvhline(3, 0, ' ', width);
    draw_text(3, 0, status_width - 1, "Status");
    draw_text(3, name_x, name_width - 1, "Package");
    draw_text(3, version_x, version_width - 1, "Version");
    draw_text(3, arch_x, arch_width - 1, "Arch");
    attroff(A_REVERSE);

    for (size_t i = 0; i < page && first + i < rows.size(); ++i) {
      const auto &row = rows[first + i];
      const int y = static_cast<int>(i) + TABLE_FIRST_ROW;
      const bool active = first + i == selected;
      if (active) {
        attron(A_REVERSE);
        mvhline(y, 0, ' ', width);
      }
      if (row.installed && !active && colors) {
        attron(COLOR_PAIR(INSTALLED_COLOR));
      }
      draw_text(y, 0, status_width - 1, row_status(row));
      if (row.installed && !active && colors) {
        attroff(COLOR_PAIR(INSTALLED_COLOR));
      }
      draw_text(y, name_x, name_width - 1, row.package.name);
      draw_text(y, version_x, version_width - 1, row.package.display_version());
      draw_text(y, arch_x, arch_width - 1, row.package.arch);
      if (active) {
        attroff(A_REVERSE);
      }
    }

    if (height >= SUMMARY_MIN_HEIGHT) {
      draw_summary(height - FOOTER_HEIGHT - SUMMARY_HEIGHT, width);
    }

    draw_text(height - 2, 0, width, notice);
    if (editing) {
      draw_bar(height - 1, width, " Enter: Search  Esc: Cancel");
    } else if (worker.valid()) {
      draw_bar(height - 1, width, " q: Quit");
    } else {
      const std::string shortcuts = "/:Search i:Installed a:All u:Unneeded g:Upgradable r:Refresh Enter:Info q:Quit";
      if (shortcuts.size() < static_cast<size_t>(width)) {
        draw_bar(height - 1, width, shortcuts);
      } else {
        draw_bar(height - 1, width, "/:Find i:Inst a:All u:Unneed g:Up r:Ref Enter q");
      }
    }

    // Draw the search last to leave the cursor at the input while editing.
    draw_search(width);
  }
};

} // namespace

int
main()
{
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    std::cerr << "dnfui-tui requires an interactive terminal.\n";
    return 1;
  }

  dnfui_i18n_init();
  try {
    tui::BackendDiagnostics diagnostics;
    // Restore the terminal before waiting for any outstanding backend work.
    PackageBrowser browser;
    tui::Terminal terminal;
    if (!terminal.valid()) {
      std::cerr << "Could not initialize the terminal.\n";
      return 1;
    }
    browser.run(terminal.colors_enabled());
    browser.cancel();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
