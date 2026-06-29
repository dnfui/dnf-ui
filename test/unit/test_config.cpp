// -----------------------------------------------------------------------------
// Config file tests
// Covers saved key value pairs and fallback behavior for invalid saved values.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "config.hpp"
#include "ui/package_table/package_table_columns.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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

// -----------------------------------------------------------------------------
// Return the default visible package table column ids from the shared metadata.
// -----------------------------------------------------------------------------
std::set<std::string>
default_visible_package_table_columns()
{
  std::set<std::string> visible;
  for (const auto &column : package_table_column_definitions()) {
    if (column.default_visible) {
      visible.insert(column.id);
    }
  }

  return visible;
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
// Verify that package table columns use the default visibility when no setting exists.
// -----------------------------------------------------------------------------
TEST_CASE("Package table columns use default visibility when missing")
{
  reset_test_config_file();

  REQUIRE(package_table_load_visible_column_ids() == default_visible_package_table_columns());
  REQUIRE(package_table_column_is_visible("package"));
  REQUIRE_FALSE(package_table_column_is_visible("release"));
  REQUIRE_FALSE(package_table_column_is_visible("unknown-column"));
}

// -----------------------------------------------------------------------------
// Verify that saved hidden package table columns are restored.
// -----------------------------------------------------------------------------
TEST_CASE("Package table columns restore saved hidden ids")
{
  reset_test_config_file();
  config_save_map({ { "package_table_hidden_columns", "summary" } });

  std::set<std::string> visible = package_table_load_visible_column_ids();

  REQUIRE(visible.count("summary") == 0);
  REQUIRE(visible.count("package") == 1);
  REQUIRE(visible.count("release") == 1);
}

// -----------------------------------------------------------------------------
// Verify that unknown package table column ids are ignored.
// -----------------------------------------------------------------------------
TEST_CASE("Package table columns ignore unknown hidden ids")
{
  reset_test_config_file();
  config_save_map({ { "package_table_hidden_columns", "summary,unknown-column" } });

  std::set<std::string> visible = package_table_load_visible_column_ids();

  REQUIRE(visible.count("summary") == 0);
  REQUIRE(visible.count("package") == 1);
  REQUIRE(visible.count("unknown-column") == 0);
}

// -----------------------------------------------------------------------------
// Verify that saving visible package table columns stores hidden ids.
// -----------------------------------------------------------------------------
TEST_CASE("Package table columns save visible ids as hidden settings")
{
  reset_test_config_file();

  package_table_save_visible_column_ids({ "package", "version" });

  std::set<std::string> visible = package_table_load_visible_column_ids();
  std::map<std::string, std::string> config = config_load_map();

  REQUIRE(visible == std::set<std::string>({ "package", "version" }));
  REQUIRE(config.count("package_table_hidden_columns") == 1);
  REQUIRE(config.count("package_table_columns") == 0);
}

// -----------------------------------------------------------------------------
// Verify that the final visible package table column cannot be hidden.
// -----------------------------------------------------------------------------
TEST_CASE("Package table columns keep one column visible")
{
  std::set<std::string> visible = { "package" };

  REQUIRE_FALSE(package_table_update_visible_column_ids(visible, "package", false));
  REQUIRE(visible == std::set<std::string>({ "package" }));

  REQUIRE(package_table_update_visible_column_ids(visible, "version", true));
  REQUIRE(package_table_update_visible_column_ids(visible, "package", false));
  REQUIRE(visible == std::set<std::string>({ "version" }));
}

// -----------------------------------------------------------------------------
// Verify that unknown package table column ids cannot change visibility.
// -----------------------------------------------------------------------------
TEST_CASE("Package table columns reject unknown visibility changes")
{
  std::set<std::string> visible = { "package" };

  REQUIRE_FALSE(package_table_update_visible_column_ids(visible, "unknown-column", true));
  REQUIRE(visible == std::set<std::string>({ "package" }));
}

// -----------------------------------------------------------------------------
// Verify that the old visible-column key is migrated to the hidden-column key.
// -----------------------------------------------------------------------------
TEST_CASE("Package table columns migrate old visible settings")
{
  reset_test_config_file();
  config_save_map({ { "package_table_columns", "package,version,unknown-column" } });

  std::set<std::string> visible = package_table_load_visible_column_ids();
  std::map<std::string, std::string> config = config_load_map();

  REQUIRE(visible == std::set<std::string>({ "package", "version" }));
  REQUIRE(config.count("package_table_hidden_columns") == 1);
  REQUIRE(config.count("package_table_columns") == 0);
}

// -----------------------------------------------------------------------------
// Verify that resetting package table columns restores the default set.
// -----------------------------------------------------------------------------
TEST_CASE("Package table columns reset to defaults")
{
  reset_test_config_file();

  package_table_save_visible_column_ids({ "package" });
  package_table_reset_visible_column_ids();

  REQUIRE(package_table_load_visible_column_ids() == default_visible_package_table_columns());
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
