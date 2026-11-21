// -----------------------------------------------------------------------------
// system_info.hpp
// Read-only system details for diagnostic snapshots.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

struct SystemInfo {
  std::string os_version;
  std::string kernel_version;
  std::string architecture;
  std::string cpu_model;
  std::uint64_t total_ram_bytes = 0;
  unsigned int logical_processors = 0;
  std::uint64_t pid = 0;
};

std::string system_info_parse_cpu_model(const std::string &cpuinfo);
SystemInfo system_info_read();

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
