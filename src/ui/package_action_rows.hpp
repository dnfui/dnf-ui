// -----------------------------------------------------------------------------
// src/ui/package_action_rows.hpp
// Package action row resolver
//
// A visible package row can mean two things when an update exists:
//   - the installed package currently on disk
//   - the available package that would be installed by an upgrade
//
// The UI needs both package IDs.
// Upgrade uses the available package ID.
// Remove and reinstall use the installed package ID.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"

struct PackageActionRows {
  PackageInstallState state = PackageInstallState::AVAILABLE;
  bool install_is_upgrade = false;
  bool has_install_row = false;
  bool has_installed_row = false;
  // Fast UI check only. This does not prove that reinstall is available from repositories.
  bool can_try_reinstall = false;
  PackageRow install_row;
  PackageRow installed_row;
};

// -----------------------------------------------------------------------------
// Resolve package IDs for action buttons without running libdnf queries.
// This is used from GTK selection and context-menu code.
// -----------------------------------------------------------------------------
PackageActionRows package_action_rows_for_selection(const PackageRow &selected);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
