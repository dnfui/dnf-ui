// -----------------------------------------------------------------------------
// src/upgrade/daemon_upgrade_target.hpp
// Package target reported by dnf5daemon's upgrade list
// -----------------------------------------------------------------------------
#pragma once

#include <string>

struct DaemonUpgradeTarget {
  std::string name;
  std::string arch;
  std::string epoch;
  std::string version;
  std::string release;
  std::string nevra;
  std::string full_nevra;
  std::string repo_id;
  std::string summary;

  std::string name_arch_key() const
  {
    return name + "\n" + arch;
  }

  std::string upgrade_spec() const
  {
    return name + "." + arch;
  }
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
