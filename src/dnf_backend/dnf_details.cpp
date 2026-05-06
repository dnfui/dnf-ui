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
// Fetch and format detailed info for one exact NEVRA, including package
// identity, repo, size, install reason, summary, and description. Installed
// matches are preferred because rpmdb metadata is authoritative for the current
// system.
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_package_info(const std::string &pkg_nevra)
{
  auto [base, guard, generation] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);

  // Exact NEVRA match only; the UI passes full package identifiers.
  query.filter_nevra(pkg_nevra);

  if (query.empty()) {
    return "No details found for " + pkg_nevra;
  }

  // Prefer installed package if available.
  libdnf5::rpm::PackageQuery installed(query);
  installed.filter_installed();

  // Select installed if found, otherwise use available.
  libdnf5::rpm::PackageQuery best_candidate = installed.empty() ? query : installed;

  // Keep only the latest version (highest EVR).
  best_candidate.filter_latest_evr();

  auto pkg = *best_candidate.begin();
  PackageRow selected_row = make_package_row(pkg);

  // Show the installed version when the selected row is an update candidate.
  libdnf5::rpm::PackageQuery installed_by_name(base);
  installed_by_name.filter_name(pkg.get_name(), libdnf5::sack::QueryCmp::EQ);
  installed_by_name.filter_installed();

  PackageRow installed_row;
  bool have_installed_counterpart = false;
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

  std::ostringstream oss;
  oss << "Name: " << pkg.get_name() << "\n"
      << "Package ID: " << pkg.get_nevra() << "\n"
      << "Version: " << pkg.get_version() << "\n"
      << "Release: " << pkg.get_release() << "\n"
      << "Arch: " << pkg.get_arch() << "\n"
      << "Repo: " << pkg.get_repo_id() << "\n"
      << "Install Size: " << format_package_size(static_cast<unsigned long long>(pkg.get_install_size())) << "\n";

  if (pkg.is_installed()) {
    oss << "Install Reason: " << dnf_backend_install_reason_to_string(selected_row.install_reason) << "\n";
  }

  unsigned long long download_size = static_cast<unsigned long long>(pkg.get_download_size());
  if (download_size > 0) {
    oss << "Download Size: " << format_package_size(download_size) << "\n";
  }

  if (have_installed_counterpart && installed_row.nevra != selected_row.nevra) {
    oss << "Installed Version: " << installed_row.display_version() << "\n";
  }

  oss << "\n"
      << "Summary:\n"
      << pkg.get_summary() << "\n\n"
      << "Description:\n"
      << pkg.get_description();

  return oss.str();
}

// -----------------------------------------------------------------------------
// Return newline-separated file paths for an installed package. Large file
// lists can overwhelm GTK clipboard transfer, so callers can pass a
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
  query.filter_installed();

  if (query.empty()) {
    DNFUI_TRACE("Backend file list not installed nevra=%s", pkg_nevra.c_str());
    return "File list available only for installed packages.";
  }

  query.filter_latest_evr();
  auto pkg = *query.begin();

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
// Retrieve dependency information for one exact NEVRA and format it for the
// package details Dependencies tab. Installed packages also include a narrow
// "Required By" section based on installed reverse dependencies.
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

  // Prefer the installed copy: its rpmdb metadata is always present and
  // authoritative. Fall back to any available repo match if not installed.
  libdnf5::rpm::PackageQuery installed(query);
  installed.filter_installed();
  libdnf5::rpm::PackageQuery &best = installed.empty() ? query : installed;
  auto pkg = *best.begin();
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
