#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <gio/gio.h>

#include <cstdlib>
#include <set>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Reset backend test globals to their default state.
// -----------------------------------------------------------------------------
inline void
reset_backend_globals()
{
  dnf_backend_testonly_clear_installed_snapshot();
}

// -----------------------------------------------------------------------------
// Return backend search options for a test case.
// -----------------------------------------------------------------------------
inline DnfBackendSearchOptions
backend_search_options(bool search_in_description, bool exact_match)
{
  return {
    .search_in_description = search_in_description,
    .exact_match = exact_match,
  };
}

// -----------------------------------------------------------------------------
// Return the NEVRA set from package rows.
// -----------------------------------------------------------------------------
inline std::set<std::string>
package_row_nevras(const std::vector<PackageRow> &rows)
{
  std::set<std::string> nevras;
  for (const auto &row : rows) {
    nevras.insert(row.nevra);
  }

  return nevras;
}

// -----------------------------------------------------------------------------
// Connect directly to a private test bus address.
// -----------------------------------------------------------------------------
inline GDBusConnection *
connect_to_test_bus(const char *bus_address, GError **error)
{
  return g_dbus_connection_new_for_address_sync(
      bus_address,
      static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr,
      nullptr,
      error);
}

struct ScopedEnvVar {
  // -----------------------------------------------------------------------------
  // Set one environment variable and remember its old state.
  // -----------------------------------------------------------------------------
  explicit ScopedEnvVar(const char *key, const char *value)
      : key(key ? key : "")
  {
    const char *existing = this->key.empty() ? nullptr : std::getenv(this->key.c_str());
    if (existing) {
      had_old_value = true;
      old_value = existing;
    }

    if (!this->key.empty()) {
      setenv(this->key.c_str(), value ? value : "", 1);
    }
  }

  // -----------------------------------------------------------------------------
  // Restore the environment variable to its old state.
  // -----------------------------------------------------------------------------
  ~ScopedEnvVar()
  {
    if (key.empty()) {
      return;
    }

    if (had_old_value) {
      setenv(key.c_str(), old_value.c_str(), 1);
    } else {
      unsetenv(key.c_str());
    }
  }

  std::string key;
  std::string old_value;
  bool had_old_value = false;
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
