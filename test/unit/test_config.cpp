// -----------------------------------------------------------------------------
// Config file tests
// Covers saved key value pairs and fallback behavior for invalid saved values.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "config.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <glib.h>
#include <glib/gstdio.h>

namespace {

// -----------------------------------------------------------------------------
// Keep config tests away from the user's real config directory.
// -----------------------------------------------------------------------------
std::filesystem::path
test_config_dir()
{
  static std::filesystem::path dir = [] {
    gchar *tmp_dir = g_dir_make_tmp("dnfui-config-test-XXXXXX", nullptr);
    REQUIRE(tmp_dir != nullptr);

    std::filesystem::path path(tmp_dir);
    g_free(tmp_dir);

    REQUIRE(g_setenv("XDG_CONFIG_HOME", path.c_str(), TRUE));
    return path;
  }();

  return dir;
}

// -----------------------------------------------------------------------------
// Return the config file used by the tests.
// -----------------------------------------------------------------------------
std::filesystem::path
test_config_file()
{
  return test_config_dir() / "dnfui.conf";
}

// -----------------------------------------------------------------------------
// Remove the test config file before each test.
// -----------------------------------------------------------------------------
void
reset_test_config_file()
{
  std::filesystem::remove(test_config_file());
}

} // namespace

// -----------------------------------------------------------------------------
// Verify that saving a config map writes the same values that loading reads back.
// -----------------------------------------------------------------------------
TEST_CASE("Config map save and load roundtrip")
{
  reset_test_config_file();

  const std::map<std::string, std::string> saved = {
    { "paned_position", "420" },
    { "window_width", "1280" },
    { "window_height", "900" },
  };

  config_save_map(saved);
  std::map<std::string, std::string> loaded = config_load_map();

  REQUIRE(loaded == saved);
}

// -----------------------------------------------------------------------------
// Verify that comments and malformed lines do not become config keys.
// -----------------------------------------------------------------------------
TEST_CASE("Config loader ignores comments and malformed lines")
{
  reset_test_config_file();
  std::filesystem::create_directories(test_config_dir());

  std::ofstream file(test_config_file());
  REQUIRE(file.good());
  file << "# comment\n";
  file << "missing-separator\n";
  file << "valid=value\n";
  file << "another=two=parts\n";
  file.close();

  std::map<std::string, std::string> loaded = config_load_map();

  REQUIRE(loaded.size() == 2);
  REQUIRE(loaded["valid"] == "value");
  REQUIRE(loaded["another"] == "two=parts");
}

// -----------------------------------------------------------------------------
// Verify that the divider position falls back when the config file is missing.
// -----------------------------------------------------------------------------
TEST_CASE("Config paned position uses default when missing")
{
  reset_test_config_file();

  REQUIRE(config_load_paned_position() == 300);
}

// -----------------------------------------------------------------------------
// Verify that invalid divider positions are ignored.
// -----------------------------------------------------------------------------
TEST_CASE("Config paned position ignores invalid saved values")
{
  reset_test_config_file();
  config_save_map({ { "paned_position", "123abc" } });

  REQUIRE(config_load_paned_position() == 300);
}

// -----------------------------------------------------------------------------
// Verify that a valid divider position is restored.
// -----------------------------------------------------------------------------
TEST_CASE("Config paned position restores valid saved value")
{
  reset_test_config_file();
  config_save_map({ { "paned_position", "640" } });

  REQUIRE(config_load_paned_position() == 640);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
