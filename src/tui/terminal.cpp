// -----------------------------------------------------------------------------
// src/tui/terminal.cpp
// -----------------------------------------------------------------------------
#include "terminal.hpp"

#include <cerrno>
#include <cwchar>
#include <system_error>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <glib.h>

namespace tui {

BackendDiagnostics::BackendDiagnostics()
{
  if (!isatty(STDERR_FILENO)) {
    return;
  }

  log.reset(std::tmpfile());
  if (!log) {
    throw std::system_error(errno, std::generic_category(), "Could not open backend diagnostic file");
  }

  std::fflush(stderr);
  saved_fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 0);
  if (saved_fd == -1) {
    throw std::system_error(errno, std::generic_category(), "Could not save stderr");
  }
  if (dup2(fileno(log.get()), STDERR_FILENO) == -1) {
    const int error = errno;
    close(saved_fd);
    throw std::system_error(error, std::generic_category(), "Could not redirect stderr");
  }
}

BackendDiagnostics::~BackendDiagnostics()
{
  if (saved_fd == -1) {
    return;
  }

  std::fflush(stderr);
  int result;
  do {
    result = dup2(saved_fd, STDERR_FILENO);
  } while (result == -1 && errno == EINTR);

  // Write to the saved descriptor even if restoring stderr failed.
  std::rewind(log.get());
  char buffer[4096];
  size_t count;
  while ((count = std::fread(buffer, 1, sizeof(buffer), log.get())) > 0) {
    size_t offset = 0;
    while (offset < count) {
      const ssize_t written = write(saved_fd, buffer + offset, count - offset);
      if (written == -1 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        close(saved_fd);
        return;
      }
      offset += static_cast<size_t>(written);
    }
  }
  close(saved_fd);
}

void
BackendDiagnostics::FileCloser::operator()(FILE *file) const noexcept
{
  std::fclose(file);
}

Terminal::Terminal()
    : screen(newterm(nullptr, stdout, stdin))
{
  if (screen) {
    if (raw() == ERR || noecho() == ERR || keypad(stdscr, true) == ERR) {
      endwin();
      delscreen(screen);
      screen = nullptr;
      return;
    }
    wtimeout(stdscr, 100);
    curs_set(0);
    if (has_colors() && start_color() == OK && use_default_colors() == OK && COLOR_PAIRS > INSTALLED_COLOR) {
      colors = init_pair(HEADING_COLOR, COLOR_CYAN, -1) == OK && init_pair(INSTALLED_COLOR, COLOR_GREEN, -1) == OK;
    }
  }
}

Terminal::~Terminal()
{
  if (screen) {
    endwin();
    delscreen(screen);
  }
}

bool
Terminal::valid() const
{
  return screen != nullptr;
}

bool
Terminal::colors_enabled() const
{
  return colors;
}

// ERR can mean a normal input timeout or a disconnected terminal.
bool
terminal_input_closed()
{
  if (std::feof(stdin) || std::ferror(stdin)) {
    return true;
  }

  pollfd input { STDIN_FILENO, POLLIN, 0 };
  const int result = poll(&input, 1, 0);
  if (result < 0 && errno != EINTR) {
    throw std::system_error(errno, std::generic_category(), "Could not check terminal input");
  }
  return result > 0 && (input.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
}

std::wstring
wide_text(const std::string &text)
{
  std::wstring result;
  char *valid = g_utf8_make_valid(text.c_str(), text.size());
  for (const char *p = valid; *p; p = g_utf8_next_char(p)) {
    const gunichar ch = g_utf8_get_char(p);
    if (ch == '\n' || ch == '\t' || wcwidth(static_cast<wchar_t>(ch)) >= 0) {
      result.push_back(static_cast<wchar_t>(ch));
    } else {
      result.push_back(L'?');
    }
  }
  g_free(valid);
  return result;
}

// Write only complete characters that fit in the available terminal columns.
void
draw_text(int y, int x, int width, const std::wstring &text)
{
  if (width <= 0) {
    return;
  }

  std::wstring visible;
  for (wchar_t ch : text) {
    if (ch == L'\n' || ch == L'\t') {
      ch = L' ';
    }
    const int cells = wcwidth(ch);
    if (cells < 0 || cells > width) {
      break;
    }
    visible.push_back(ch);
    width -= cells;
  }
  if (!visible.empty()) {
    mvaddnwstr(y, x, visible.c_str(), static_cast<int>(visible.size()));
  }
}

void
draw_text(int y, int x, int width, const std::string &text)
{
  draw_text(y, x, width, wide_text(text));
}

std::vector<std::wstring>
wrap_text(const std::string &text, int width)
{
  std::vector<std::wstring> lines;
  std::wstring line;
  int used = 0;
  for (wchar_t ch : wide_text(text)) {
    if (ch == L'\n') {
      lines.push_back(line);
      line.clear();
      used = 0;
      continue;
    }
    if (ch == L'\t') {
      ch = L' ';
    }
    const int cells = wcwidth(ch);
    if (used + cells > width) {
      lines.push_back(line);
      line.clear();
      used = 0;
    }
    line.push_back(ch);
    used += cells;
  }
  lines.push_back(line);
  return lines;
}

void
draw_bar(int y, int width, const std::string &text)
{
  attron(A_REVERSE);
  mvhline(y, 0, ' ', width);
  draw_text(y, 0, width - 1, text);
  attroff(A_REVERSE);
}

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
