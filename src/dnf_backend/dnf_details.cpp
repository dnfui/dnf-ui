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

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <utility>

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
  std::string text = formatted ? formatted : _("Unknown");
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
// Upgradable packages can be opened from either the installed package row or the available update row.
// The details pane should describe the installed package first, then add the available update version when one exists.
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_package_info(const std::string &pkg_nevra)
{
  return dnf_backend_get_package_info(pkg_nevra, PackageDetailsContext::INSTALLED_CONTEXT, nullptr);
}

std::string
dnf_backend_get_package_info(const std::string &pkg_nevra, const PackageRow *upgrade_row_override)
{
  return dnf_backend_get_package_info(pkg_nevra, PackageDetailsContext::INSTALLED_CONTEXT, upgrade_row_override);
}

std::string
dnf_backend_get_package_info(const std::string &pkg_nevra,
                             PackageDetailsContext context,
                             const PackageRow *upgrade_row_override)
{
  PackageRow selected_row;
  unsigned long long selected_install_size = 0;
  std::string selected_summary, selected_description;

  PackageRow installed_row;
  bool have_installed_counterpart = false;
  unsigned long long installed_install_size = 0;
  std::string installed_summary, installed_description;

  PackageRow upgrade_row;
  PackageRow upgrade_baseline_row;
  bool have_upgrade = false;
  bool have_upgrade_baseline = false;
  bool selected_installed_is_newest = true;
  unsigned long long upgrade_download_size = 0;

  auto [base, guard] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);
  query.filter_nevra(pkg_nevra);

  if (query.empty()) {
    return dnfui_i18n_format(_("No details found for %s"), pkg_nevra.c_str());
  }

  const bool use_installed_context = context == PackageDetailsContext::INSTALLED_CONTEXT;
  libdnf5::rpm::PackageQuery best_candidate(query);
  if (use_installed_context) {
    libdnf5::rpm::PackageQuery installed_q(query);
    installed_q.filter_installed();
    if (!installed_q.empty()) {
      best_candidate = installed_q;
    }
  }
  best_candidate.filter_latest_evr();
  auto pkg = *best_candidate.begin();

  selected_row = make_package_row(pkg);
  selected_install_size = static_cast<unsigned long long>(pkg.get_install_size());
  selected_summary = pkg.get_summary();
  selected_description = pkg.get_description();

  if (use_installed_context) {
    if (pkg.is_installed()) {
      installed_row = selected_row;
      installed_install_size = selected_install_size;
      installed_summary = selected_summary;
      installed_description = selected_description;
      have_installed_counterpart = true;

      libdnf5::rpm::PackageQuery installed_by_name(base);
      installed_by_name.filter_name(pkg.get_name(), libdnf5::sack::QueryCmp::EQ);
      installed_by_name.filter_installed();
      for (auto installed_pkg : installed_by_name) {
        if (installed_pkg.get_arch() != pkg.get_arch()) {
          continue;
        }
        PackageRow row = make_package_row(installed_pkg);
        if (!have_upgrade_baseline || libdnf5::rpm::evrcmp(row, upgrade_baseline_row) > 0) {
          upgrade_baseline_row = row;
          have_upgrade_baseline = true;
        }
      }
      if (have_upgrade_baseline && libdnf5::rpm::evrcmp(selected_row, upgrade_baseline_row) < 0) {
        selected_installed_is_newest = false;
      }
    } else {
      // Find the installed package with the same name and architecture.
      // This gives available update rows the same installed context as Latest only mode.
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
          upgrade_baseline_row = row;
          have_upgrade_baseline = true;
        }
      }
    }
  }

  if (have_upgrade_baseline && selected_installed_is_newest && upgrade_row_override &&
      libdnf5::rpm::evrcmp(*upgrade_row_override, upgrade_baseline_row) > 0) {
    upgrade_row = *upgrade_row_override;
    have_upgrade = true;
  } else if (have_upgrade_baseline && selected_installed_is_newest) {
    // Find the newest available package with the same name and architecture.
    // It is an upgrade only when its EVR is newer than the installed package.
    libdnf5::rpm::PackageQuery available_by_name(base);
    available_by_name.filter_name(upgrade_baseline_row.name, libdnf5::sack::QueryCmp::EQ);
    available_by_name.filter_available();
    available_by_name.filter_latest_evr();

    for (auto available_pkg : available_by_name) {
      if (available_pkg.get_arch() != upgrade_baseline_row.arch) {
        continue;
      }
      PackageRow row = make_package_row(available_pkg);
      if (libdnf5::rpm::evrcmp(row, upgrade_baseline_row) <= 0) {
        continue;
      }
      if (!have_upgrade || libdnf5::rpm::evrcmp(row, upgrade_row) > 0) {
        upgrade_row = row;
        upgrade_download_size = static_cast<unsigned long long>(available_pkg.get_download_size());
        have_upgrade = true;
      }
    }
  }

  // Installed context uses installed metadata for the header when the package is installed.
  // This keeps version, repo, install size, and install reason consistent across different lists.
  const bool show_installed_context = use_installed_context && have_installed_counterpart;
  const PackageRow &display_row = show_installed_context ? installed_row : selected_row;
  const unsigned long long display_install_size =
      show_installed_context ? installed_install_size : selected_install_size;
  const std::string &display_summary = show_installed_context ? installed_summary : selected_summary;
  const std::string &display_description = show_installed_context ? installed_description : selected_description;

  std::ostringstream oss;
  oss << _("Name") << ": " << display_row.name << "\n"
      << _("Package ID") << ": " << display_row.nevra << "\n"
      << _("Version") << ": " << display_row.version << "\n"
      << _("Release") << ": " << display_row.release << "\n"
      << _("Arch") << ": " << display_row.arch << "\n"
      << _("Repo") << ": " << display_row.repo << "\n";

  if (show_installed_context) {
    oss << _("Installed From") << ": " << installed_row.installed_from_repo << "\n";
  }

  oss << _("Install Size") << ": " << format_package_size(display_install_size) << "\n";

  if (show_installed_context) {
    oss << _("Install Reason") << ": " << dnf_backend_install_reason_to_string(installed_row.install_reason) << "\n";
  }

  if (upgrade_download_size > 0) {
    oss << _("Download Size") << ": " << format_package_size(upgrade_download_size) << "\n";
  }

  if (have_upgrade) {
    oss << _("Installed Version") << ": " << installed_row.display_version() << "\n";
    oss << _("Upgradable Version") << ": " << upgrade_row.display_version() << "\n";
  }

  oss << "\n" << _("Summary") << ":\n" << display_summary << "\n\n" << _("Description") << ":\n" << display_description;

  return oss.str();
}

// -----------------------------------------------------------------------------
// Return newline-separated file paths for the installed package behind one selected NEVRA.
// Available update rows use the currently installed package with the same name and architecture.
//
// Large file lists can overwhelm GTK clipboard transfer.
// Callers can pass a positive max_files_for_display to append a truncation notice after that many visible entries.
// Passing 0 returns the full list.
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_installed_package_files(const std::string &pkg_nevra, size_t max_files_for_display)
{
  return dnf_backend_get_installed_package_files(
      pkg_nevra, PackageDetailsContext::INSTALLED_CONTEXT, max_files_for_display);
}

std::string
dnf_backend_get_installed_package_files(const std::string &pkg_nevra,
                                        PackageDetailsContext context,
                                        size_t max_files_for_display,
                                        std::optional<std::vector<std::string>> *file_paths)
{
  if (file_paths) {
    *file_paths = std::nullopt;
  }

  DNFUI_TRACE("Backend file list start nevra=%s max_display=%zu", pkg_nevra.c_str(), max_files_for_display);
  auto [base, guard] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);

  query.filter_nevra(pkg_nevra);
  if (query.empty()) {
    DNFUI_TRACE("Backend file list package not found nevra=%s", pkg_nevra.c_str());
    return _("File list available only for installed packages.");
  }

  std::string installed_nevra;

  // If the selected NEVRA is already installed, use it directly.
  libdnf5::rpm::PackageQuery exact_installed(query);
  exact_installed.filter_installed();

  if (!exact_installed.empty()) {
    exact_installed.filter_latest_evr();
    auto installed_pkg = *exact_installed.begin();
    installed_nevra = installed_pkg.get_nevra();
  } else if (context == PackageDetailsContext::INSTALLED_CONTEXT) {
    libdnf5::rpm::PackageQuery selected_query(query);
    selected_query.filter_latest_evr();
    auto selected_pkg = *selected_query.begin();

    // Available update packages do not own the installed file list.
    // Look up the installed package with the same name and architecture instead.
    libdnf5::rpm::PackageQuery installed_by_name(base);
    installed_by_name.filter_name(selected_pkg.get_name(), libdnf5::sack::QueryCmp::EQ);
    installed_by_name.filter_installed();

    // Match the selected package architecture and keep the newest installed package
    // if more than one installed row exists.
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
    return _("File list available only for installed packages.");
  }

  // Load the resolved installed package before reading its recorded file list.
  libdnf5::rpm::PackageQuery installed_files_query(base);
  installed_files_query.filter_nevra(installed_nevra);
  installed_files_query.filter_installed();

  // If the installed package cannot be found here, use the same message as for packages without an installed file list.
  if (installed_files_query.empty()) {
    DNFUI_TRACE("Backend file list installed package not found nevra=%s installed_nevra=%s",
                pkg_nevra.c_str(),
                installed_nevra.c_str());
    return _("File list available only for installed packages.");
  }

  installed_files_query.filter_latest_evr();
  auto pkg = *installed_files_query.begin();
  std::vector<std::string> paths = pkg.get_files();

  std::ostringstream files;
  size_t file_count = 0;
  size_t displayed_count = 0;
  const bool should_truncate = max_files_for_display > 0;

  for (const auto &f : paths) {
    ++file_count;
    if (!should_truncate || displayed_count < max_files_for_display) {
      files << f << "\n";
      ++displayed_count;
    }
  }

  if (file_paths) {
    *file_paths = std::move(paths);
  }

  std::string result = files.str();
  if (result.empty()) {
    result = _("No files recorded for this installed package.");
  } else if (should_truncate && file_count > max_files_for_display) {
    const size_t hidden_count = file_count - displayed_count;
    files << "\n"
          << dnfui_i18n_format_count(
                 hidden_count, "--- %zu more file not shown ---", "--- %zu more files not shown ---")
          << "\n"
          << _("--- Use the package manager CLI for complete list ---") << "\n";
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
// Retrieve dependency information for the installed package behind one selected
// NEVRA and format it for the package details Dependencies tab.
//
// If the selected NEVRA is an available update, use the currently installed
// package with the same name and architecture.
// -----------------------------------------------------------------------------
std::string
dnf_backend_get_package_deps(const std::string &pkg_nevra)
{
  return dnf_backend_get_package_deps(pkg_nevra, PackageDetailsContext::INSTALLED_CONTEXT);
}

std::string
dnf_backend_get_package_deps(const std::string &pkg_nevra, PackageDetailsContext context)
{
  auto [base, guard] = BaseManager::instance().acquire_read();
  libdnf5::rpm::PackageQuery query(base);

  query.filter_nevra(pkg_nevra);

  if (query.empty()) {
    return _("No dependency information found for this package.");
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

    if (context == PackageDetailsContext::INSTALLED_CONTEXT) {
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
  }

  libdnf5::rpm::PackageQuery dependency_query(base);
  dependency_query.filter_nevra(dependency_nevra);
  if (dependency_nevra_is_installed) {
    dependency_query.filter_installed();
  }

  if (dependency_query.empty()) {
    return _("No dependency information found for this package.");
  }

  dependency_query.filter_latest_evr();
  auto pkg = *dependency_query.begin();
  std::set<std::string> required_by_nevras = collect_installed_reverse_dependency_nevras(base, pkg);

  std::ostringstream out;

  auto list_field = [&](const char *title, const auto &items) {
    out << title << ":\n";
    if (items.empty()) {
      out << "  " << _("(none)") << "\n\n";
      return;
    }
    for (const auto &i : items) {
      out << "  " << i.to_string() << "\n";
    }
    out << "\n";
  };

  list_field(_("Requires"), pkg.get_requires());
  out << _("Required By") << ":\n";
  if (!pkg.is_installed()) {
    out << "  " << _("(installed packages only)") << "\n\n";
  } else if (required_by_nevras.empty()) {
    out << "  " << _("(none)") << "\n\n";
  } else {
    for (const auto &nevra : required_by_nevras) {
      out << "  " << nevra << "\n";
    }
    out << "\n";
  }
  list_field(_("Provides"), pkg.get_provides());
  list_field(_("Conflicts"), pkg.get_conflicts());
  list_field(_("Obsoletes"), pkg.get_obsoletes());

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
    return _("No changelog entries found.");
  }

  std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
    return left.get_timestamp() > right.get_timestamp();
  });

  for (const auto &entry : entries) {
    std::time_t ts = static_cast<std::time_t>(entry.get_timestamp());
    std::tm tm_buf {};
    localtime_r(&ts, &tm_buf);

    out << _("Date") << ": " << std::put_time(&tm_buf, "%Y-%m-%d") << "\n"
        << _("Author") << ": " << entry.get_author() << "\n"
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
      return _("No changelog available.");
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
    return _("No changelog available.");
  }

  available_changelog.filter_latest_evr();
  auto pkg = *available_changelog.begin();
  return format_changelog_entries(pkg);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
