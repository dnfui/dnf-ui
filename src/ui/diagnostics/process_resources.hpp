// -----------------------------------------------------------------------------
// process_resources.hpp
// Read-only resource measurements for the DNF UI process.
// -----------------------------------------------------------------------------
#pragma once

#include <glib.h>

#include <cstdint>
#include <string>

struct ProcessMemorySample {
  std::uint64_t resident_bytes = 0;
  std::uint64_t peak_bytes = 0;
  std::uint64_t anonymous_bytes = 0;
  std::uint64_t file_bytes = 0;
  std::uint64_t shared_bytes = 0;
};

struct ProcessCpuSample {
  std::uint64_t cpu_nanoseconds = 0;
  gint64 monotonic_microseconds = 0;
};

bool process_resources_parse_memory_status(const std::string &status, ProcessMemorySample &out);
bool process_resources_read_memory(ProcessMemorySample &out);
bool process_resources_read_cpu(ProcessCpuSample &out);
double process_resources_cpu_fraction(const ProcessCpuSample &previous,
                                      const ProcessCpuSample &current,
                                      unsigned int logical_cpus);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
