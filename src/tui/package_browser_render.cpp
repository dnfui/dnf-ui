// -----------------------------------------------------------------------------
// src/tui/package_browser_render.cpp
// -----------------------------------------------------------------------------
#include "package_browser.hpp"

#include "terminal.hpp"

#include <algorithm>
#include <cwchar>

namespace tui {
namespace {

  constexpr int SUMMARY_MIN_HEIGHT = 12;
  constexpr int SUMMARY_HEIGHT = 3;
  constexpr int TABLE_FIRST_ROW = 4;
  constexpr int FOOTER_HEIGHT = 2;

  const char *view_name(PackageView view)
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

} // namespace

// Reserve room for the summary only when there is enough space for the table.
int
PackageBrowser::list_page_rows(int height)
{
  const int summary = height >= SUMMARY_MIN_HEIGHT ? SUMMARY_HEIGHT : 0;
  return std::max(1, height - TABLE_FIRST_ROW - FOOTER_HEIGHT - summary);
}

const char *
PackageBrowser::row_status(const DisplayRow &row)
{
  if (row.upgrade) {
    return "Upgradable";
  }
  return row.installed ? "Installed" : "Available";
}

void
PackageBrowser::draw()
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

  std::string heading = " DNF UI - Pending: " + std::to_string(pending.size());
  if (offline_state == tui::OfflineState::PREPARED) {
    heading += " | Reboot required";
  } else if (offline_state == tui::OfflineState::UNKNOWN) {
    heading += " | Offline status unknown";
  }
  draw_bar(0, width, heading);

  if (screen == Screen::APPLYING) {
    draw_text(1, 0, width, "Transaction in progress");
    const auto lines = wrap_text(notice, width);
    for (size_t i = 0; i < lines.size() && i < static_cast<size_t>(height - 4); ++i) {
      draw_text(static_cast<int>(i) + 2, 0, width, lines[i]);
    }
    draw_bar(height - 1, width, " Wait for the transaction to finish");
  } else if (screen == Screen::PACKAGE_DETAILS || screen == Screen::PENDING_DETAILS || screen == Screen::PREVIEW ||
             screen == Screen::CONFIRM_APPLY || screen == Screen::APPLY_RESULT) {
    if (screen == Screen::PREVIEW || screen == Screen::CONFIRM_APPLY || screen == Screen::APPLY_RESULT) {
      draw_text(1, 0, width, notice);
    }
    const auto lines = wrap_text(details, width);
    const size_t page = static_cast<size_t>(height - 4);
    const size_t last = lines.size() > page ? lines.size() - page : 0;
    details_first = std::min(details_first, last);
    for (size_t i = 0; i < page && details_first + i < lines.size(); ++i) {
      draw_text(static_cast<int>(i) + 2, 0, width, lines[details_first + i]);
    }
    if (screen == Screen::CONFIRM_APPLY) {
      draw_bar(height - 1, width, " y: Confirm  n/Esc: Cancel");
    } else if (screen == Screen::PREVIEW && prepared) {
      draw_bar(height - 1,
               width,
               prepared->requires_offline() ? " a:Prepare for reboot Esc:Back q:Quit"
                                            : " a:Apply Up/Down:Scroll Esc:Back q:Quit");
    } else if (screen == Screen::APPLY_RESULT) {
      draw_bar(height - 1, width, " Esc: Reload packages  q: Quit");
    } else {
      draw_bar(height - 1, width, " Up/Down: Scroll  Esc: Back  q: Quit");
    }
  } else if (screen == Screen::ACTION_MENU) {
    draw_action_menu(height, width);
  } else if (screen == Screen::PENDING) {
    draw_pending(height, width);
  } else {
    draw_list(height, width);
  }
  if (screen == Screen::SEARCH) {
    curs_set(1);
  } else {
    curs_set(0);
  }
  refresh();
}

void
PackageBrowser::draw_action_menu(int height, int width)
{
  const auto &row = rows[selected];
  const auto choices = tui::pending_choices(row);
  draw_text(1, 0, width, row.package.nevra);
  if (choices.can_install) {
    draw_text(
        2, 0, width, std::string("1: Mark for ") + tui::pending_action_name(tui::install_side_type(choices.rows)));
  } else if (choices.rows.install_is_upgrade && !row.upgrade) {
    draw_text(2, 0, width, "Use the Upgradable view to mark upgrades.");
  }
  if (choices.can_remove) {
    draw_text(3, 0, width, "2: Mark for removal");
  }
  if (choices.can_reinstall) {
    draw_text(4, 0, width, "3: Mark for reinstall");
  }
  const auto action = std::find_if(pending.begin(), pending.end(), [&](const tui::PendingMark &item) {
    return item.action.nevra == row.package.nevra;
  });
  if (action != pending.end()) {
    draw_text(5, 0, width, "x: Unmark");
  } else if (!choices.can_install && !choices.can_remove && !choices.can_reinstall &&
             !(choices.rows.install_is_upgrade && !row.upgrade)) {
    draw_text(5, 0, width, "No actions available for this package.");
  }
  draw_bar(height - 1, width, " Esc: Back  q: Quit");
}

void
PackageBrowser::draw_pending(int height, int width)
{
  const int action_width = 12;
  const size_t page = static_cast<size_t>(height - 5);
  if (pending_selected < pending_first) {
    pending_first = pending_selected;
  } else if (pending_selected - pending_first >= page) {
    pending_first = pending_selected - page + 1;
  }
  const size_t last_first = pending.size() > page ? pending.size() - page : 0;
  pending_first = std::min(pending_first, last_first);

  draw_text(1, 0, width, "Pending actions (!: check required; Enter: info)");
  attron(A_REVERSE);
  mvhline(2, 0, ' ', width);
  draw_text(2, 0, action_width - 1, "Action");
  draw_text(2, action_width, width - action_width, "Package");
  attroff(A_REVERSE);

  if (pending.empty()) {
    draw_text(3, 0, width, "No pending actions");
  }
  for (size_t i = 0; i < page && pending_first + i < pending.size(); ++i) {
    const auto &mark = pending[pending_first + i];
    const auto &action = mark.action;
    const int y = static_cast<int>(i) + 3;
    const bool active = pending_first + i == pending_selected;
    if (active) {
      attron(A_REVERSE);
      mvhline(y, 0, ' ', width);
    }
    const std::string label = (mark.has_issue() ? "!" : "") + std::string(tui::pending_action_name(action.type));
    draw_text(y, 0, action_width - 1, label);
    draw_text(y, action_width, width - action_width, action.nevra);
    if (active) {
      attroff(A_REVERSE);
    }
  }
  if (worker.valid()) {
    draw_text(height - 2, 0, width, notice);
    draw_bar(height - 1, width, " q: Quit");
  } else {
    draw_bar(height - 1, width, "v:Preview x:Unmark c:Clear Enter:Info Esc:Back q");
  }
}

// Show the applied search separately from the editable input so cancelling
// an edit does not change the query displayed above the package table.
void
PackageBrowser::draw_search(int width)
{
  attron(A_BOLD);
  draw_text(1, 0, width, "Search: ");
  attroff(A_BOLD);

  if (screen != Screen::SEARCH) {
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

void
PackageBrowser::draw_summary(int y, int width)
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

void
PackageBrowser::draw_list(int height, int width)
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
  if (!worker.valid() && screen != Screen::SEARCH) {
    const std::string actions = "m:Mark p:Pending";
    draw_text(2, width - static_cast<int>(actions.size()) - 1, static_cast<int>(actions.size()), actions);
  }

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
    const auto action = std::find_if(pending.begin(), pending.end(), [&](const tui::PendingMark &item) {
      return item.action.nevra == row.package.nevra;
    });
    if (action != pending.end()) {
      draw_text(y,
                0,
                status_width - 1,
                std::string(action->has_issue() ? "!" : "*") + tui::pending_action_name(action->action.type));
    } else {
      draw_text(y, 0, status_width - 1, row_status(row));
    }
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
  if (screen == Screen::SEARCH) {
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

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
