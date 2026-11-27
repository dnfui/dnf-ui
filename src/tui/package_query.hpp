// -----------------------------------------------------------------------------
// src/tui/package_query.hpp
// -----------------------------------------------------------------------------
#pragma once

#include "upgrade_query.hpp"

namespace tui {

enum class PackageView {
  INSTALLED,
  ALL,
  UNNEEDED,
  UPGRADABLE,
  SEARCH,
};

QueryResult query_packages(PackageView view,
                           const std::string &term,
                           bool refresh_snapshot,
                           bool refresh_metadata,
                           GCancellable *cancel,
                           const std::shared_ptr<std::atomic<bool>> &cancel_flag);

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
