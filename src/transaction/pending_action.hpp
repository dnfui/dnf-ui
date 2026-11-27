// -----------------------------------------------------------------------------
// src/transaction/pending_action.hpp
// Shared pending package action
// -----------------------------------------------------------------------------
#pragma once

#include <string>

// -----------------------------------------------------------------------------
// Pending actions for the mark, review, and apply workflow
// -----------------------------------------------------------------------------
struct PendingAction {
  enum Type { INSTALL, UPGRADE, DOWNGRADE, REMOVE, REINSTALL } type;
  // Package row shown in the pending list.
  std::string nevra;
  // Package spec sent to the transaction client.
  std::string transaction_spec;
  // Package name and architecture used to replace conflicting pending actions.
  std::string package_key = {};
  // True when DNF allows multiple exact installs for this package stream.
  bool installonly = false;
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
