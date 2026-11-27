// -----------------------------------------------------------------------------
// test/unit/test_tui_shutdown.cpp
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "tui/shutdown.hpp"

#include <initializer_list>

TEST_CASE("TUI termination signals request orderly shutdown")
{
  for (const int signal : { SIGHUP, SIGTERM, SIGINT }) {
    struct sigaction before { };
    REQUIRE(sigaction(signal, nullptr, &before) == 0);
    {
      tui::ShutdownSignals shutdown;
      REQUIRE_FALSE(shutdown.requested());
      REQUIRE(raise(signal) == 0);
      REQUIRE(shutdown.requested());
    }
    struct sigaction after { };
    REQUIRE(sigaction(signal, nullptr, &after) == 0);
    REQUIRE(after.sa_handler == before.sa_handler);
  }
}
