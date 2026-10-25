// -----------------------------------------------------------------------------
// test_main_window_css_resource.cpp
// The main window stylesheet must be embedded in the application build.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include <gio/gio.h>

#include <string>

TEST_CASE("Main window stylesheet is available as a compiled resource", "[resources]")
{
  GBytes *bytes =
      g_resources_lookup_data("/io/github/dnfui/dnfui/main_window.css", G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
  REQUIRE(bytes != nullptr);

  gsize size = 0;
  const char *data = static_cast<const char *>(g_bytes_get_data(bytes, &size));
  const std::string css(data, size);

  REQUIRE(css.find("label.status-bar {") != std::string::npos);
  REQUIRE(css.find(".prepared-updates-banner {") != std::string::npos);
  REQUIRE(css.find(".package-table-view row:selected {") != std::string::npos);

  g_bytes_unref(bytes);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
