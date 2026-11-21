// -----------------------------------------------------------------------------
// process_resources.cpp
// Read process memory from procfs and CPU time from the process clock.
// -----------------------------------------------------------------------------
#include "ui/diagnostics/process_resources.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <time.h>

static bool
parse_kilobytes(std::string_view line, std::string_view field, std::uint64_t &bytes)
{
  if (!line.starts_with(field)) {
    return false;
  }

  std::istringstream value { std::string(line.substr(field.size())) };
  std::uint64_t kib = 0;
  std::string unit;
  if (!(value >> kib >> unit) || unit != "kB" || kib > std::numeric_limits<std::uint64_t>::max() / 1024) {
    return false;
  }

  value >> std::ws;
  if (!value.eof()) {
    return false;
  }
  bytes = kib * 1024;

  return true;
}

// -----------------------------------------------------------------------------
// Parse the procfs status fields used by the monitor.
// Leave the output untouched if a required field is missing or malformed.
// -----------------------------------------------------------------------------
bool
process_resources_parse_memory_status(const std::string &status, ProcessMemorySample &out)
{
  ProcessMemorySample sample;
  bool have_resident = false;
  bool have_peak = false;
  bool have_anonymous = false;
  bool have_file = false;
  bool have_shared = false;

  std::istringstream input(status);
  std::string line;
  while (std::getline(input, line)) {
    if (line.starts_with("VmRSS:")) {
      if (!parse_kilobytes(line, "VmRSS:", sample.resident_bytes)) {
        return false;
      }
      have_resident = true;
    } else if (line.starts_with("VmHWM:")) {
      if (!parse_kilobytes(line, "VmHWM:", sample.peak_bytes)) {
        return false;
      }
      have_peak = true;
    } else if (line.starts_with("RssAnon:")) {
      if (!parse_kilobytes(line, "RssAnon:", sample.anonymous_bytes)) {
        return false;
      }
      have_anonymous = true;
    } else if (line.starts_with("RssFile:")) {
      if (!parse_kilobytes(line, "RssFile:", sample.file_bytes)) {
        return false;
      }
      have_file = true;
    } else if (line.starts_with("RssShmem:")) {
      if (!parse_kilobytes(line, "RssShmem:", sample.shared_bytes)) {
        return false;
      }
      have_shared = true;
    }
  }

  if (!have_resident || !have_peak || !have_anonymous || !have_file || !have_shared) {
    return false;
  }
  out = sample;

  return true;
}

// -----------------------------------------------------------------------------
// Read the current process's resident memory measurements.
// -----------------------------------------------------------------------------
bool
process_resources_read_memory(ProcessMemorySample &out)
{
  std::ifstream status("/proc/self/status");
  if (!status) {
    return false;
  }

  std::ostringstream contents;
  contents << status.rdbuf();
  return !status.bad() && process_resources_parse_memory_status(contents.str(), out);
}

// -----------------------------------------------------------------------------
// Sample total CPU time across every thread in this process.
// -----------------------------------------------------------------------------
bool
process_resources_read_cpu(ProcessCpuSample &out)
{
  timespec cpu_time {};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_time) != 0) {
    return false;
  }

  out.cpu_nanoseconds =
      static_cast<std::uint64_t>(cpu_time.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(cpu_time.tv_nsec);
  out.monotonic_microseconds = g_get_monotonic_time();

  return true;
}

// -----------------------------------------------------------------------------
// Return CPU usage as a fraction of all logical processor capacity.
// -----------------------------------------------------------------------------
double
process_resources_cpu_fraction(const ProcessCpuSample &previous,
                               const ProcessCpuSample &current,
                               unsigned int logical_cpus)
{
  if (logical_cpus == 0 || current.monotonic_microseconds <= previous.monotonic_microseconds ||
      current.cpu_nanoseconds < previous.cpu_nanoseconds) {
    return 0.0;
  }

  const double elapsed_nanoseconds =
      static_cast<double>(current.monotonic_microseconds - previous.monotonic_microseconds) * 1000.0;
  const double used_nanoseconds = static_cast<double>(current.cpu_nanoseconds - previous.cpu_nanoseconds);

  return std::clamp(used_nanoseconds / elapsed_nanoseconds / logical_cpus, 0.0, 1.0);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
