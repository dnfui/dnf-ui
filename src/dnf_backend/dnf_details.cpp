// -----------------------------------------------------------------------------
// src/dnf_backend/dnf_details.cpp
// Package detail text queries
//
// Formats info, file lists, dependencies, and changelog entries for the GTK details pane.
// These helpers are read-only libdnf5 queries and do not mutate the installed-package UI cache.
// -----------------------------------------------------------------------------
#include "dnf_backend/dnf_internal.hpp"

#include "dnf_backend/base_manager.hpp"
#include "debug_trace.hpp"
#include "i18n.hpp"

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
// Collect installed packages whose requires match capabilities provided by the selected installed package.
// This keeps reverse dependency reporting narrow and focused on the current system state.
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
// The Info tab describes the selected package row.
// If an installed counterpart exists, the text can still include installed and upgradable version context.
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_package_info(const std::string &pkg_nevra)
{
  PackageRow selected_row;
  unsigned long long selected_install_size = 0;
  std::string selected_summary, selected_description;

  PackageRow installed_row;
  bool have_installed_counterpart = false;

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
  const bool selected_is_installed = !installed_q.empty();
  libdnf5::rpm::PackageQuery best_candidate = installed_q.empty() ? query : installed_q;
  best_candidate.filter_latest_evr();
  auto pkg = *best_candidate.begin();

  selected_row = make_package_row(pkg);
  selected_install_size = static_cast<unsigned long long>(pkg.get_install_size());
  selected_summary = pkg.get_summary();
  selected_description = pkg.get_description();

  // Find the installed package with the same name and architecture.
  // This gives the details pane one answer for both an installed row and its update row.
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

  std::ostringstream oss;
  oss << _("Name") << ": " << selected_row.name << "\n"
      << _("Package ID") << ": " << selected_row.nevra << "\n"
      << _("Version") << ": " << selected_row.version << "\n"
      << _("Release") << ": " << selected_row.release << "\n"
      << _("Arch") << ": " << selected_row.arch << "\n"
      << _("Repo") << ": " << selected_row.repo << "\n"
      << _("Install Size") << ": " << format_package_size(selected_install_size) << "\n";

  if (selected_is_installed) {
    oss << _("Install Reason") << ": " << dnf_backend_install_reason_to_string(installed_row.install_reason) << "\n";
  }

  if (upgrade_download_size > 0) {
    oss << _("Download Size") << ": " << format_package_size(upgrade_download_size) << "\n";
  }

  if (have_upgrade) {
    oss << _("Installed Version") << ": " << installed_row.display_version() << "\n";
    oss << _("Upgradable Version") << ": " << upgrade_row.display_version() << "\n";
  }

  oss << "\n"
      << _("Summary") << ":\n"
      << selected_summary << "\n\n"
      << _("Description") << ":\n"
      << selected_description;

  return oss.str();
}

// -----------------------------------------------------------------------------
// Return newline-separated file paths for one installed package.
// Available package rows do not have an installed file list.
//
// Large file lists can overwhelm GTK clipboard transfer.
// Callers can pass a positive max_files_for_display to append a truncation notice after that many visible entries.
// Passing 0 returns the full list.
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
    return _("File list available only for installed packages.");
  }

  libdnf5::rpm::PackageQuery exact_installed(query);
  exact_installed.filter_installed();

  if (exact_installed.empty()) {
    DNFUI_TRACE("Backend file list not installed nevra=%s", pkg_nevra.c_str());
    return _("File list available only for installed packages.");
  }

  exact_installed.filter_latest_evr();
  auto pkg = *exact_installed.begin();

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
    result = "No files recorded for this installed package.";
  } else if (should_truncate && file_count > max_files_for_display) {
    const size_t hidden_count = file_count - displayed_count;
    files << "\n--- " << hidden_count << " more file" << (hidden_count == 1 ? "" : "s") << " not shown ---\n"
          << "--- Use the package manager CLI for complete list ---\n";
    result = files.str();
  }

  DNFUI_TRACE("Backend file list done nevra=%s total=%zu displayed=%zu bytes=%zu",
              pkg_nevra.c_str(),
              file_count,
              displayed_count,
              result.size());

  return result;
}

// -----------------------------------------------------------------------------
// Retrieve dependency information for one selected NEVRA and format it for the package details Dependencies tab.
// Reverse dependency reporting is limited to installed packages because it describes the current system.
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

  libdnf5::rpm::PackageQuery exact_installed(query);
  exact_installed.filter_installed();

  const bool selected_is_installed = !exact_installed.empty();
  query.filter_latest_evr();
  auto pkg = *query.begin();
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
  if (!selected_is_installed) {
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
// Retrieve and format changelog entries for one exact NEVRA.
// Installed package metadata is used first because rpmdb changelog metadata is local.
// Available packages use a temporary Base with repository changelog metadata.
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_package_changelog(const std::string &pkg_nevra)
{
  {
    auto read = BaseManager::instance().acquire_read();
    libdnf5::rpm::PackageQuery query(read.base);

    query.filter_nevra(pkg_nevra);
    if (query.empty()) {
      return "No changelog available.";
    }

    // If the selected NEVRA is already installed, use it directly.
    libdnf5::rpm::PackageQuery exact_installed(query);
    exact_installed.filter_installed();

    if (!exact_installed.empty()) {
      exact_installed.filter_latest_evr();
      auto pkg = *exact_installed.begin();
      return format_changelog_entries(pkg);
    }
  }

  auto changelog_base = BaseManager::instance().build_changelog_base();
  libdnf5::rpm::PackageQuery available_changelog(*changelog_base);

  available_changelog.filter_nevra(pkg_nevra);

  if (available_changelog.empty()) {
    return "No changelog available.";
  }

  available_changelog.filter_latest_evr();
  auto pkg = *available_changelog.begin();
  return format_changelog_entries(pkg);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
