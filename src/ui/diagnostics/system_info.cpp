// -----------------------------------------------------------------------------
// system_info.cpp
// Read static operating system, memory, and processor information.
// -----------------------------------------------------------------------------
#include "ui/diagnostics/system_info.hpp"

#include <glib.h>

#include <fstream>
#include <sstream>
#include <string_view>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

// -----------------------------------------------------------------------------
// Remove leading and trailing whitespace from a procfs field.
// -----------------------------------------------------------------------------
static std::string_view
trim_field(std::string_view value)
{
  const size_t start = value.find_first_not_of(" \t\r");
  if (start == std::string_view::npos) {
    return {};
  }
  const size_t end = value.find_last_not_of(" \t\r");

  return value.substr(start, end - start + 1);
}

// -----------------------------------------------------------------------------
// Prefer the CPU model name, falling back to ARM processor descriptions.
// -----------------------------------------------------------------------------
std::string
system_info_parse_cpu_model(const std::string &cpuinfo)
{
  std::istringstream input(cpuinfo);
  std::string line;
  std::string processor;
  std::string hardware;

  while (std::getline(input, line)) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }

    const std::string_view key = trim_field(std::string_view(line).substr(0, colon));
    const std::string_view value = trim_field(std::string_view(line).substr(colon + 1));
    if (value.empty()) {
      continue;
    }

    if (key == "model name") {
      return std::string(value);
    }
    if (key == "Processor" && processor.empty()) {
      processor = value;
    } else if (key == "Hardware" && hardware.empty()) {
      hardware = value;
    }
  }

  return !processor.empty() ? processor : hardware;
}

// -----------------------------------------------------------------------------
// Read static system details once when the monitor opens.
// -----------------------------------------------------------------------------
SystemInfo
system_info_read()
{
  SystemInfo result;

  gchar *os_name = g_get_os_info(G_OS_INFO_KEY_PRETTY_NAME);
  if (os_name) {
    result.os_version = os_name;
    g_free(os_name);
  }

  struct sysinfo memory { };
  if (sysinfo(&memory) == 0) {
    result.total_ram_bytes = static_cast<std::uint64_t>(memory.totalram) * memory.mem_unit;
  }

  result.logical_processors = static_cast<unsigned int>(g_get_num_processors());
  result.pid = static_cast<std::uint64_t>(getpid());

  struct utsname kernel { };
  if (uname(&kernel) == 0) {
    result.kernel_version = kernel.release;
    result.architecture = kernel.machine;
  }

  std::ifstream cpuinfo("/proc/cpuinfo");
  if (cpuinfo) {
    std::ostringstream contents;
    contents << cpuinfo.rdbuf();
    if (!cpuinfo.bad()) {
      result.cpu_model = system_info_parse_cpu_model(contents.str());
    }
  }

  return result;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
