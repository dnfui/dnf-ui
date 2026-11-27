// -----------------------------------------------------------------------------
// src/tui/terminal.hpp
// -----------------------------------------------------------------------------
#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#define NCURSES_NOMACROS
#include <ncurses.h>

namespace tui {

inline constexpr short HEADING_COLOR = 1;
inline constexpr short INSTALLED_COLOR = 2;

// Keep backend warnings off the ncurses screen and replay them after shutdown.
// Leave stderr alone when the caller has already redirected it.
class BackendDiagnostics {
  public:
  BackendDiagnostics();
  ~BackendDiagnostics();

  BackendDiagnostics(const BackendDiagnostics &) = delete;
  BackendDiagnostics &operator=(const BackendDiagnostics &) = delete;

  private:
  struct FileCloser {
    void operator()(FILE *file) const noexcept;
  };

  std::unique_ptr<FILE, FileCloser> log;
  int saved_fd = -1;
};

class Terminal {
  public:
  Terminal();
  ~Terminal();

  Terminal(const Terminal &) = delete;
  Terminal &operator=(const Terminal &) = delete;

  bool valid() const;
  bool colors_enabled() const;

  private:
  SCREEN *screen;
  bool colors = false;
};

bool terminal_input_closed();
std::wstring wide_text(const std::string &text);
void draw_text(int y, int x, int width, const std::wstring &text);
void draw_text(int y, int x, int width, const std::string &text);
std::vector<std::wstring> wrap_text(const std::string &text, int width);
void draw_bar(int y, int width, const std::string &text);

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
