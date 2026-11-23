#pragma once

#include <catch2/catch_test_macros.hpp>
#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Cache GTK initialization so headless tests do not retry a failed init.
// -----------------------------------------------------------------------------
inline bool
test_gtk_display_available()
{
  static const bool available = gtk_init_check();
  if (!available) {
    REQUIRE(g_getenv("DNFUI_TEST_REQUIRE_DISPLAY") == nullptr);
  }
  return available;
}
