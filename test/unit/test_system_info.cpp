#include "ui/diagnostics/system_info.hpp"

#include <catch2/catch_test_macros.hpp>

// -----------------------------------------------------------------------------
// Extract the CPU model from a typical x86 procfs entry.
// -----------------------------------------------------------------------------
TEST_CASE("System information reads x86 CPU model", "[resources]")
{
  const std::string cpuinfo = "processor\t: 0\n"
                              "vendor_id\t: GenuineIntel\n"
                              "model name\t: Example Processor 3.50GHz\n"
                              "processor\t: 1\n"
                              "model name\t: Example Processor 3.50GHz\n";

  REQUIRE(system_info_parse_cpu_model(cpuinfo) == "Example Processor 3.50GHz");
}

// -----------------------------------------------------------------------------
// Recognize processor descriptions that appear on ARM systems.
// -----------------------------------------------------------------------------
TEST_CASE("System information reads ARM CPU descriptions", "[resources]")
{
  const std::string cpuinfo = "processor : 0\n"
                              "Processor : ARMv7 Processor rev 4\n"
                              "Hardware : Example Board\n";

  REQUIRE(system_info_parse_cpu_model(cpuinfo) == "ARMv7 Processor rev 4");
  REQUIRE(system_info_parse_cpu_model("Hardware : Example Board\n") == "Example Board");
  REQUIRE(system_info_parse_cpu_model("processor : 0\n") == "");
}

// -----------------------------------------------------------------------------
// Read kernel, architecture, and the current process ID from Linux.
// -----------------------------------------------------------------------------
TEST_CASE("System information reads kernel and process identity", "[resources]")
{
  const SystemInfo info = system_info_read();
  REQUIRE_FALSE(info.kernel_version.empty());
  REQUIRE_FALSE(info.architecture.empty());
  REQUIRE(info.pid > 0);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
