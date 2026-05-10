// -----------------------------------------------------------------------------
// src/dnf_backend/dnf_details.cpp
// Package detail text queries
//
// Formats info, file lists, dependencies, and changelog entries for the GTK
// details pane. These helpers are read-only libdnf5 queries and do not mutate
// the installed-package UI cache.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_internal.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"

#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>

#include <gio/gio.h>

#include <libdnf5/base/base.hpp>
#include <libdnf5/rpm/package_query.hpp>

using namespace dnf_backend_internal;

// -----------------------------------------------------------------------------
// Format package size in a way that is easy to read in the details pane.
// -----------------------------------------------------------------------------
static std::string
format_package_size(unsigned long long size_bytes)
{
  char *formatted = g_format_size(size_bytes);
  std::string text = formatted ? formatted : "Unknown";
  g_free(formatted);
  return text;
}

// -----------------------------------------------------------------------------
// Format the file paths recorded on one package.
// -----------------------------------------------------------------------------
static std::string
format_package_files(const libdnf5::rpm::Package &pkg,
                     size_t max_files_for_display,
                     const char *empty_message,
                     const char *intro_message = nullptr)
{
  std::ostringstream files;
  size_t file_count = 0;
  size_t displayed_count = 0;
  const bool should_truncate = max_files_for_display > 0;

  for (const auto &f : pkg.get_files()) {
    ++file_count;
    if (!should_truncate || displayed_count < max_files_for_display) {
      files << f << "\n";
      ++displayed_count;
    }
  }

  std::string result = files.str();
  if (result.empty()) {
    return empty_message;
  }

  if (should_truncate && file_count > max_files_for_display) {
    const size_t hidden_count = file_count - displayed_count;
    files << "\n--- " << hidden_count << " more file" << (hidden_count == 1 ? "" : "s") << " not shown ---\n"
          << "--- Use the package manager CLI for complete list ---\n";
    result = files.str();
  }

  if (intro_message && intro_message[0] != '\0') {
    std::ostringstream text;
    text << intro_message << "\n\n" << result;
    result = text.str();
  }

  return result;
}

// -----------------------------------------------------------------------------
// Collect installed packages whose requires match capabilities provided by the
// selected installed package. This keeps reverse dependency reporting narrow
// and focused on the current system state.
// -----------------------------------------------------------------------------
static std::set<std::string>
collect_installed_reverse_dependency_nevras(libdnf5::Base &base, const libdnf5::rpm::Package &pkg)
{
  std::set<std::string> required_by_nevras;
  if (!pkg.is_installed()) {
    return required_by_nevras;
  }

  libdnf5::rpm::PackageQuery required_by(base);
  required_by.filter_installed();
  required_by.filter_requires(pkg.get_provides());

  for (const auto &dependent_pkg : required_by) {
    if (dependent_pkg.get_nevra() == pkg.get_nevra()) {
      continue;
    }

    required_by_nevras.insert(dependent_pkg.get_nevra());
  }

  return required_by_nevras;
}

// -----------------------------------------------------------------------------
// Fetch and format details for one exact NEVRA.
//
// Upgradable packages can be opened from either the installed package row or
// the available update row. The details pane should describe the installed
// package first, then add the available update version when one exists.
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_package_info(const std::string &pkg_nevra)
{
  PackageRow selected_row;
  unsigned long long selected_install_size = 0;
  std::string selected_summary, selected_description;

  PackageRow installed_row;
  bool have_installed_counterpart = false;
  unsigned long long installed_install_size = 0;
  std::string installed_summary, installed_description;

  PackageRow upgrade_row;
  bool have_upgrade = false;
  unsigned long long upgrade_download_size = 0;

  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_nevra(pkg_nevra);

  if (query.empty()) {
    return "No details found for " + pkg_nevra;
  }

  libdnf5::rpm::PackageQuery installed_q(query);
  installed_q.filter_installed();
  libdnf5::rpm::PackageQuery best_candidate = installed_q.empty() ? query : installed_q;
  best_candidate.filter_latest_evr();
  auto pkg = *best_candidate.begin();

  selected_row = make_package_row(pkg);
  selected_install_size = static_cast<unsigned long long>(pkg.get_install_size());
  selected_summary = pkg.get_summary();
  selected_description = pkg.get_description();

  // Find the installed package with the same name and architecture. This gives
  // the details pane one answer for both an installed row and its update row.
  libdnf5::rpm::PackageQuery installed_by_name(base);
  installed_by_name.filter_name(pkg.get_name(), libdnf5::sack::QueryCmp::EQ);
  installed_by_name.filter_installed();
  for (auto installed_pkg : installed_by_name) {
    if (installed_pkg.get_arch() != pkg.get_arch()) {
      continue;
    }
    PackageRow row = make_package_row(installed_pkg);
    if (!have_installed_counterpart || libdnf5::rpm::evrcmp(row, installed_row) > 0) {
      installed_row = row;
      installed_install_size = static_cast<unsigned long long>(installed_pkg.get_install_size());
      installed_summary = installed_pkg.get_summary();
      installed_description = installed_pkg.get_description();
      have_installed_counterpart = true;
    }
  }

  // Find the newest available package with the same name and architecture.
  // It is an upgrade only when its EVR is newer than the installed package.
  if (have_installed_counterpart) {
    libdnf5::rpm::PackageQuery available_by_name(base);
    available_by_name.filter_name(installed_row.name, libdnf5::sack::QueryCmp::EQ);
    available_by_name.filter_available();
    available_by_name.filter_latest_evr();

    for (auto available_pkg : available_by_name) {
      if (available_pkg.get_arch() != installed_row.arch) {
        continue;
      }
      PackageRow row = make_package_row(available_pkg);
      if (libdnf5::rpm::evrcmp(row, installed_row) <= 0) {
        continue;
      }
      if (!have_upgrade || libdnf5::rpm::evrcmp(row, upgrade_row) > 0) {
        upgrade_row = row;
        upgrade_download_size = static_cast<unsigned long long>(available_pkg.get_download_size());
        have_upgrade = true;
      }
    }
  }

  // Use installed metadata for the header. This keeps the version, repo, install size, and install reason
  // stable when the user opens the package from any list.
  const PackageRow &display_row = have_installed_counterpart ? installed_row : selected_row;
  const unsigned long long display_install_size =
      have_installed_counterpart ? installed_install_size : selected_install_size;
  const std::string &display_summary = have_installed_counterpart ? installed_summary : selected_summary;
  const std::string &display_description = have_installed_counterpart ? installed_description : selected_description;

  std::ostringstream oss;
  oss << "Name: " << display_row.name << "\n"
      << "Package ID: " << display_row.nevra << "\n"
      << "Version: " << display_row.version << "\n"
      << "Release: " << display_row.release << "\n"
      << "Arch: " << display_row.arch << "\n"
      << "Repo: " << display_row.repo << "\n"
      << "Install Size: " << format_package_size(display_install_size) << "\n";

  if (have_installed_counterpart) {
    oss << "Install Reason: " << dnf_backend_install_reason_to_string(installed_row.install_reason) << "\n";
  }

  if (upgrade_download_size > 0) {
    oss << "Download Size: " << format_package_size(upgrade_download_size) << "\n";
  }

  if (have_upgrade) {
    oss << "Installed Version: " << installed_row.display_version() << "\n";
    oss << "Upgradable Version: " << upgrade_row.display_version() << "\n";
  }

  oss << "\n"
      << "Summary:\n"
      << display_summary << "\n\n"
      << "Description:\n"
      << display_description;

  return oss.str();
}

// -----------------------------------------------------------------------------
// Return newline-separated file paths for the installed package behind one
// selected NEVRA. If the selected NEVRA is an available update, use the
// currently installed package with the same name and architecture.
//
// Large file lists can overwhelm GTK clipboard transfer, so callers can pass a
// positive max_files_for_display to append a truncation notice after that many
// visible entries. Passing 0 returns the full list.
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_installed_package_files(const std::string &pkg_nevra, size_t max_files_for_display)
{
  DNFUI_TRACE("Backend file list start nevra=%s max_display=%zu", pkg_nevra.c_str(), max_files_for_display);
  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);

  query.filter_nevra(pkg_nevra);
  if (query.empty()) {
    DNFUI_TRACE("Backend file list package not found nevra=%s", pkg_nevra.c_str());
    return "File list available only for installed packages.";
  }

  std::string installed_nevra;

  // If the selected NEVRA is already installed, use it directly.
  libdnf5::rpm::PackageQuery exact_installed(query);
  exact_installed.filter_installed();

  if (!exact_installed.empty()) {
    exact_installed.filter_latest_evr();
    auto installed_pkg = *exact_installed.begin();
    installed_nevra = installed_pkg.get_nevra();
  } else {
    libdnf5::rpm::PackageQuery selected_query(query);
    selected_query.filter_latest_evr();
    auto selected_pkg = *selected_query.begin();

    // Available update packages do not own the installed file list. Look up the
    // installed package with the same name and architecture instead.
    libdnf5::rpm::PackageQuery installed_by_name(base);
    installed_by_name.filter_name(selected_pkg.get_name(), libdnf5::sack::QueryCmp::EQ);
    installed_by_name.filter_installed();

    // Match the selected package architecture and keep the newest installed
    // package if more than one installed row exists.
    PackageRow installed_row;
    bool have_installed_row = false;

    for (auto installed_pkg : installed_by_name) {
      if (installed_pkg.get_arch() != selected_pkg.get_arch()) {
        continue;
      }

      PackageRow row = make_package_row(installed_pkg);
      if (!have_installed_row || libdnf5::rpm::evrcmp(row, installed_row) > 0) {
        installed_row = row;
        installed_nevra = row.nevra;
        have_installed_row = true;
      }
    }
  }

  if (installed_nevra.empty()) {
    DNFUI_TRACE("Backend file list not installed nevra=%s", pkg_nevra.c_str());
    return "File list available only for installed packages.";
  }

  // Load the resolved installed package before reading its recorded file list.
  libdnf5::rpm::PackageQuery installed_files_query(base);
  installed_files_query.filter_nevra(installed_nevra);
  installed_files_query.filter_installed();

  // This should still find the installed package. If it does not, show the
  // normal message for packages without an installed file list.
  if (installed_files_query.empty()) {
    DNFUI_TRACE("Backend file list installed package not found nevra=%s installed_nevra=%s",
                pkg_nevra.c_str(),
                installed_nevra.c_str());
    return "File list available only for installed packages.";
  }

  installed_files_query.filter_latest_evr();
  auto pkg = *installed_files_query.begin();

  std::string result =
      format_package_files(pkg, max_files_for_display, "No files recorded for this installed package.");

  DNFUI_TRACE("Backend file list done nevra=%s bytes=%zu", pkg_nevra.c_str(), result.size());

  return result;
}

// -----------------------------------------------------------------------------
// Return true when the selected NEVRA is available and no matching installed
// package already provides the normal installed file list.
// -----------------------------------------------------------------------------
bool
dnf_backend_can_load_available_package_files(const std::string &pkg_nevra)
{
  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_nevra(pkg_nevra);
  query.filter_available();

  if (query.empty()) {
    return false;
  }

  query.filter_latest_evr();
  auto selected_pkg = *query.begin();

  libdnf5::rpm::PackageQuery installed_by_name(base);
  installed_by_name.filter_name(selected_pkg.get_name(), libdnf5::sack::QueryCmp::EQ);
  installed_by_name.filter_installed();

  for (auto installed_pkg : installed_by_name) {
    if (installed_pkg.get_arch() == selected_pkg.get_arch()) {
      return false;
    }
  }

  return true;
}

// -----------------------------------------------------------------------------
// Return repository file metadata for an available package.
// This is only used after the user asks for it manually.
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_available_package_files(const std::string &pkg_nevra, size_t max_files_for_display)
{
  DNFUI_TRACE("Backend available file list start nevra=%s max_display=%zu", pkg_nevra.c_str(), max_files_for_display);
  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_nevra(pkg_nevra);
  query.filter_available();

  if (query.empty()) {
    DNFUI_TRACE("Backend available file list package not found nevra=%s", pkg_nevra.c_str());
    return "Repository file metadata is available only for repository packages.";
  }

  query.filter_latest_evr();
  auto pkg = *query.begin();
  std::string result = format_package_files(pkg,
                                            max_files_for_display,
                                            "No repository file metadata is available for this package.",
                                            "Repository file metadata may not list every file in the package.");
  DNFUI_TRACE("Backend available file list done nevra=%s bytes=%zu", pkg_nevra.c_str(), result.size());
  return result;
}

// -----------------------------------------------------------------------------
// Retrieve dependency information for the installed package behind one selected
// NEVRA and format it for the package details Dependencies tab.
//
// If the selected NEVRA is an available update, use the currently installed
// package with the same name and architecture.
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_package_deps(const std::string &pkg_nevra)
{
  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);

  query.filter_nevra(pkg_nevra);

  if (query.empty()) {
    return "No dependency information found for this package.";
  }

  std::string dependency_nevra;
  bool dependency_nevra_is_installed = false;

  // If the selected NEVRA is already installed, use it directly.
  libdnf5::rpm::PackageQuery exact_installed(query);
  exact_installed.filter_installed();
  if (!exact_installed.empty()) {
    exact_installed.filter_latest_evr();
    auto installed_pkg = *exact_installed.begin();
    dependency_nevra = installed_pkg.get_nevra();
    dependency_nevra_is_installed = true;
  } else {
    libdnf5::rpm::PackageQuery selected_query(query);
    selected_query.filter_latest_evr();
    auto selected_pkg = *selected_query.begin();
    dependency_nevra = selected_pkg.get_nevra();

    // Available update packages do not describe the current system state.
    // Look up the installed package with the same name and architecture instead.
    libdnf5::rpm::PackageQuery installed_by_name(base);
    installed_by_name.filter_name(selected_pkg.get_name(), libdnf5::sack::QueryCmp::EQ);
    installed_by_name.filter_installed();

    // Match the selected package architecture and keep the newest installed
    // package if more than one installed row exists.
    PackageRow installed_row;
    bool have_installed_row = false;
    for (auto installed_pkg : installed_by_name) {
      if (installed_pkg.get_arch() != selected_pkg.get_arch()) {
        continue;
      }

      PackageRow row = make_package_row(installed_pkg);
      if (!have_installed_row || libdnf5::rpm::evrcmp(row, installed_row) > 0) {
        installed_row = row;
        dependency_nevra = row.nevra;
        dependency_nevra_is_installed = true;
        have_installed_row = true;
      }
    }
  }

  libdnf5::rpm::PackageQuery dependency_query(base);
  dependency_query.filter_nevra(dependency_nevra);
  if (dependency_nevra_is_installed) {
    dependency_query.filter_installed();
  }

  if (dependency_query.empty()) {
    return "No dependency information found for this package.";
  }

  dependency_query.filter_latest_evr();
  auto pkg = *dependency_query.begin();
  std::set<std::string> required_by_nevras = collect_installed_reverse_dependency_nevras(base, pkg);

  std::ostringstream out;

  auto list_field = [&](const char *title, const auto &items) {
    out << title << ":\n";
    if (items.empty()) {
      out << "  (none)\n\n";
      return;
    }
    for (const auto &i : items) {
      out << "  " << i.to_string() << "\n";
    }
    out << "\n";
  };

  list_field("Requires", pkg.get_requires());
  out << "Required By:\n";
  if (!pkg.is_installed()) {
    out << "  (installed packages only)\n\n";
  } else if (required_by_nevras.empty()) {
    out << "  (none)\n\n";
  } else {
    for (const auto &nevra : required_by_nevras) {
      out << "  " << nevra << "\n";
    }
    out << "\n";
  }
  list_field("Provides", pkg.get_provides());
  list_field("Conflicts", pkg.get_conflicts());
  list_field("Obsoletes", pkg.get_obsoletes());

  return out.str();
}

// -----------------------------------------------------------------------------
// Format changelog entries for display in the details pane.
// -----------------------------------------------------------------------------
static std::string
format_changelog_entries(libdnf5::rpm::Package pkg)
{
  std::ostringstream out;

  auto entries = pkg.get_changelogs();
  if (entries.empty()) {
    return "No changelog entries found.";
  }

  for (const auto &entry : entries) {
    std::time_t ts = static_cast<std::time_t>(entry.get_timestamp());
    std::tm tm_buf {};
    localtime_r(&ts, &tm_buf);

    out << "Date: " << std::put_time(&tm_buf, "%Y-%m-%d") << "\n"
        << "Author: " << entry.get_author() << "\n"
        << entry.get_text() << "\n\n";
  }

  return out.str();
}

// -----------------------------------------------------------------------------
// Retrieve and format changelog entries for one exact NEVRA. Installed package
// metadata is preferred because rpmdb entries often contain a fuller local
// history than repository metadata. If the package is not installed, fall back
// to a temporary Base with repo "other" metadata so normal queries do not keep
// changelog metadata resident.
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_package_changelog(const std::string &pkg_nevra)
{
  {
    auto read = BaseManager::instance().acquire_read();
    libdnf5::rpm::PackageQuery installed(read.base);

    // Try the installed rpmdb first: it does not need repo "other" metadata.
    installed.filter_nevra(pkg_nevra);
    installed.filter_installed();

    if (!installed.empty()) {
      // Keep the newest installed match if more than one package matches.
      installed.filter_latest_evr();
      auto pkg = *installed.begin();
      return format_changelog_entries(pkg);
    }
  }

  // Load repo "other" metadata only when no installed package was found.
  auto changelog_base = BaseManager::instance().acquire_changelog_read();
  libdnf5::rpm::PackageQuery query(*changelog_base.base);

  query.filter_nevra(pkg_nevra);

  if (query.empty()) {
    return "No changelog available.";
  }

  // Keep the newest available match if more than one package matches.
  query.filter_latest_evr();
  auto pkg = *query.begin();

  return format_changelog_entries(pkg);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
