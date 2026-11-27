// -----------------------------------------------------------------------------
// src/tui/shutdown.cpp
// -----------------------------------------------------------------------------
#include "shutdown.hpp"

#include <atomic>
#include <cerrno>
#include <system_error>

namespace tui {
namespace {

  constexpr int signals[] = { SIGHUP, SIGTERM, SIGINT };
  std::atomic<bool> shutdown_requested { false };
  static_assert(std::atomic<bool>::is_always_lock_free);

  void request_shutdown(int)
  {
    shutdown_requested.store(true, std::memory_order_relaxed);
  }

} // namespace

ShutdownSignals::ShutdownSignals()
{
  shutdown_requested.store(false, std::memory_order_relaxed);
  struct sigaction action { };
  action.sa_handler = request_shutdown;
  action.sa_flags = SA_RESTART;
  sigemptyset(&action.sa_mask);
  for (int i = 0; i < 3; ++i) {
    if (sigaction(signals[i], &action, &previous[i]) == -1) {
      const int error = errno;
      while (i > 0) {
        --i;
        sigaction(signals[i], &previous[i], nullptr);
      }
      throw std::system_error(error, std::generic_category(), "Could not install shutdown signal handler");
    }
  }
}

ShutdownSignals::~ShutdownSignals()
{
  for (int i = 0; i < 3; ++i) {
    sigaction(signals[i], &previous[i], nullptr);
  }
}

bool
ShutdownSignals::requested() const
{
  return shutdown_requested.load(std::memory_order_relaxed);
}

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
