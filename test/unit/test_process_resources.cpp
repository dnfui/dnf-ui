#include "ui/diagnostics/process_resources.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// -----------------------------------------------------------------------------
// Read the memory fields from a procfs-style status snapshot.
// -----------------------------------------------------------------------------
TEST_CASE("Resource monitor parses resident and peak memory", "[resources]")
{
  const std::string status =
      "Name:\tdnfui\nVmHWM:\t4096 kB\nVmRSS:\t2048 kB\nRssAnon:\t1536 kB\nRssFile:\t512 kB\nRssShmem:\t0 kB\n";

  ProcessMemorySample sample;
  REQUIRE(process_resources_parse_memory_status(status, sample));
  REQUIRE(sample.resident_bytes == 2048ULL * 1024);
  REQUIRE(sample.peak_bytes == 4096ULL * 1024);
  REQUIRE(sample.anonymous_bytes == 1536ULL * 1024);
  REQUIRE(sample.file_bytes == 512ULL * 1024);
  REQUIRE(sample.shared_bytes == 0);
}

// -----------------------------------------------------------------------------
// Reject incomplete and invalid memory fields without replacing the last sample.
// -----------------------------------------------------------------------------
TEST_CASE("Resource monitor rejects incomplete memory snapshots", "[resources]")
{
  ProcessMemorySample sample;
  sample.resident_bytes = 123;

  REQUIRE_FALSE(process_resources_parse_memory_status("VmRSS:\t100 kB\nVmHWM:\t200 kB\n", sample));
  REQUIRE(sample.resident_bytes == 123);

  const std::string malformed = "VmRSS:\tbad kB\nVmHWM:\t200 kB\nRssAnon:\t60 kB\nRssFile:\t40 kB\nRssShmem:\t0 kB\n";
  REQUIRE_FALSE(process_resources_parse_memory_status(malformed, sample));
  REQUIRE(sample.resident_bytes == 123);

  const std::string overflow =
      "VmRSS:\t18446744073709551615 kB\nVmHWM:\t200 kB\nRssAnon:\t60 kB\nRssFile:\t40 kB\nRssShmem:\t0 kB\n";
  REQUIRE_FALSE(process_resources_parse_memory_status(overflow, sample));
}

// -----------------------------------------------------------------------------
// CPU usage is normalized against the capacity of all logical processors.
// -----------------------------------------------------------------------------
TEST_CASE("Resource monitor calculates process CPU usage", "[resources]")
{
  const ProcessCpuSample before { 500000000, 1000000 };
  const ProcessCpuSample after { 900000000, 2000000 };

  REQUIRE(process_resources_cpu_fraction(before, after, 1) == Catch::Approx(0.4));
  REQUIRE(process_resources_cpu_fraction(before, after, 4) == Catch::Approx(0.1));
  REQUIRE(process_resources_cpu_fraction(before, after, 0) == Catch::Approx(0.0));
  REQUIRE(process_resources_cpu_fraction(after, before, 4) == Catch::Approx(0.0));

  const ProcessCpuSample busy { 3000000000, 2000000 };
  REQUIRE(process_resources_cpu_fraction(before, busy, 1) == Catch::Approx(1.0));
}

// -----------------------------------------------------------------------------
// Read measurements on a live Linux system.
// -----------------------------------------------------------------------------
TEST_CASE("Resource monitor reads process counters", "[resources]")
{
  ProcessMemorySample memory;
  ProcessCpuSample cpu;
  REQUIRE(process_resources_read_memory(memory));
  REQUIRE(memory.resident_bytes > 0);
  REQUIRE(process_resources_read_cpu(cpu));
  REQUIRE(cpu.monotonic_microseconds > 0);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
