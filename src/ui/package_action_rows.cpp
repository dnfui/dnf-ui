// -----------------------------------------------------------------------------
// src/ui/package_action_rows.cpp
// Package action row resolver
//
// Keeps the package ID rules for upgrade, install, remove, and reinstall in one
// place. This file must not run libdnf queries because it is used while GTK is
// updating buttons and opening context menus.
// -----------------------------------------------------------------------------
#include "package_action_rows.hpp"

// -----------------------------------------------------------------------------
// Resolve package IDs for action buttons without running libdnf queries.
// -----------------------------------------------------------------------------
PackageActionRows
package_action_rows_for_selection(const PackageRow &selected)
{
  PackageActionRows rows;
  rows.state = dnf_backend_get_package_install_state(selected);
  rows.install_is_upgrade = rows.state == PackageInstallState::UPGRADEABLE;
  rows.install_row = selected;
  rows.installed_row = selected;

  const bool selected_is_installed = dnf_backend_is_package_installed_exact(selected);
  rows.has_installed_row = selected_is_installed;

  // Upgrade actions need the available package ID, not always the visible row ID.
  if (rows.install_is_upgrade) {
    if (selected_is_installed) {
      // Installed-list rows store the matching available upgrade package ID when the backend annotates them.
      rows.has_install_row = !selected.repo_candidate_nevra.empty();
      rows.install_row.nevra = selected.repo_candidate_nevra;
    } else {
      // Upgradable-list rows are already the available upgrade package.
      // The installed package ID comes from the installed snapshot.
      rows.has_install_row = true;
      rows.has_installed_row = dnf_backend_get_installed_package_row_by_name_arch(selected, rows.installed_row);
    }
    rows.can_try_reinstall = rows.has_installed_row;
    return rows;
  }

  // Plain available packages can only be installed.
  if (rows.state == PackageInstallState::AVAILABLE) {
    rows.has_install_row = true;
  }

  // Reinstall needs an installed package that is still available from repositories.
  rows.can_try_reinstall = rows.has_installed_row && rows.state != PackageInstallState::LOCAL_ONLY &&
      rows.state != PackageInstallState::INSTALLED_NEWER_THAN_REPO;

  return rows;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
