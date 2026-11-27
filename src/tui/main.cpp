// -----------------------------------------------------------------------------
// src/tui/main.cpp
// Terminal frontend for the DNF backend
// -----------------------------------------------------------------------------
#include "i18n.hpp"
#include "package_browser.hpp"
#include "shutdown.hpp"
#include "terminal.hpp"

#include <exception>
#include <iostream>

#include <unistd.h>

int
main()
{
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    std::cerr << "dnfui-tui requires an interactive terminal.\n";
    return 1;
  }

  dnfui_i18n_init();
  try {
    // Install before ncurses so its default signal handlers do not bypass cleanup.
    tui::ShutdownSignals shutdown;
    tui::BackendDiagnostics diagnostics;
    // Restore the terminal before waiting for any outstanding backend work.
    tui::PackageBrowser browser;
    tui::Terminal terminal;
    if (!terminal.valid()) {
      std::cerr << "Could not initialize the terminal.\n";
      return 1;
    }
    browser.run(terminal.colors_enabled(), shutdown);
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
