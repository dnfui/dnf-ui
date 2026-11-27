// -----------------------------------------------------------------------------
// src/tui/shutdown.hpp
// -----------------------------------------------------------------------------
#pragma once

#include <signal.h>

namespace tui {

class ShutdownSignals {
  public:
  ShutdownSignals();
  ~ShutdownSignals();

  ShutdownSignals(const ShutdownSignals &) = delete;
  ShutdownSignals &operator=(const ShutdownSignals &) = delete;

  bool requested() const;

  private:
  struct sigaction previous[3] {};
};

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
