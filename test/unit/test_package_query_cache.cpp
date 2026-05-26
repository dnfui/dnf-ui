// -----------------------------------------------------------------------------
// Package query cache tests
// Covers cache keys, generation checks, and cache invalidation.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "test_utils.hpp"
#include "ui/package_query_cache.hpp"

#include <string>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Build one small package row for cache tests.
// -----------------------------------------------------------------------------
PackageRow
make_cache_row(const std::string &nevra, const std::string &name)
{
  PackageRow row;
  row.nevra = nevra;
  row.name = name;
  row.arch = "x86_64";
  return row;
}

} // namespace

// -----------------------------------------------------------------------------
// Verify that cache keys include the search mode flags that affect results.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache key includes search options")
{
  reset_backend_globals();

  set_backend_search_options(false, false);
  REQUIRE(package_query_cache_key_for("bash") == "name:contains:bash");

  set_backend_search_options(false, true);
  REQUIRE(package_query_cache_key_for("bash") == "name:exact:bash");

  set_backend_search_options(true, false);
  REQUIRE(package_query_cache_key_for("bash") == "desc:contains:bash");

  set_backend_search_options(true, true);
  REQUIRE(package_query_cache_key_for("bash") == "desc:exact:bash");
}

// -----------------------------------------------------------------------------
// Verify that cached rows are returned only for the same key and generation.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache returns rows for matching key and generation")
{
  package_query_cache_clear();
  const uint64_t base_epoch = 3;
  const uint64_t cache_epoch = package_query_cache_current_epoch();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
    make_cache_row("demo-libs-1-1.x86_64", "demo-libs"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, 7, base_epoch, cache_epoch, stored);

  REQUIRE(package_query_cache_lookup(key, 7, base_epoch, cache_epoch, loaded));
  REQUIRE(loaded.size() == 2);
  REQUIRE(loaded[0].nevra == "demo-1-1.x86_64");
  REQUIRE(loaded[1].nevra == "demo-libs-1-1.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that stale generation lookups are rejected and removed from cache.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache rejects and removes stale generations")
{
  package_query_cache_clear();
  const uint64_t base_epoch = 3;
  const uint64_t cache_epoch = package_query_cache_current_epoch();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, 7, base_epoch, cache_epoch, stored);

  REQUIRE_FALSE(package_query_cache_lookup(key, 8, base_epoch, cache_epoch, loaded));
  REQUIRE_FALSE(package_query_cache_lookup(key, 7, base_epoch, cache_epoch, loaded));
}

// -----------------------------------------------------------------------------
// Verify that clearing the query cache removes stored package rows.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache clear removes stored rows")
{
  package_query_cache_clear();
  const uint64_t base_epoch = 3;
  const uint64_t cache_epoch = package_query_cache_current_epoch();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, 7, base_epoch, cache_epoch, stored);
  package_query_cache_clear();

  REQUIRE_FALSE(package_query_cache_lookup(key, 7, base_epoch, cache_epoch, loaded));
}

// -----------------------------------------------------------------------------
// Verify that clearing the cache advances the cache epoch as well.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache clear advances cache epoch")
{
  package_query_cache_clear();
  const uint64_t before = package_query_cache_current_epoch();

  package_query_cache_clear();

  REQUIRE(package_query_cache_current_epoch() > before);
}

// -----------------------------------------------------------------------------
// Verify that stale cache-epoch rows are rejected and removed from cache.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache rejects rows from an older cache epoch")
{
  package_query_cache_clear();
  const uint64_t base_epoch = 3;
  const uint64_t cache_epoch = package_query_cache_current_epoch();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, 7, base_epoch, cache_epoch, stored);
  package_query_cache_clear();

  REQUIRE_FALSE(package_query_cache_lookup(key, 7, base_epoch, cache_epoch, loaded));
}

// -----------------------------------------------------------------------------
// Verify that cached rows are rejected after the shared Base lifetime changes.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache rejects rows from an older Base epoch")
{
  package_query_cache_clear();
  const uint64_t cache_epoch = package_query_cache_current_epoch();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, 7, 3, cache_epoch, stored);

  REQUIRE_FALSE(package_query_cache_lookup(key, 7, 4, cache_epoch, loaded));
  REQUIRE_FALSE(package_query_cache_lookup(key, 7, 3, cache_epoch, loaded));
}

// -----------------------------------------------------------------------------
// Verify that a stale worker does not store rows back into a newer cache epoch.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache ignores store requests from an older cache epoch")
{
  package_query_cache_clear();
  const uint64_t base_epoch = 3;
  const uint64_t stale_epoch = package_query_cache_current_epoch();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_clear();
  const uint64_t current_epoch = package_query_cache_current_epoch();
  REQUIRE(current_epoch > stale_epoch);

  package_query_cache_store(key, 7, base_epoch, stale_epoch, stored);

  REQUIRE_FALSE(package_query_cache_lookup(key, 7, base_epoch, current_epoch, loaded));
}

// -----------------------------------------------------------------------------
// Verify that cached rows cannot grow without bound across many search terms.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache evicts oldest entries")
{
  package_query_cache_clear();
  const uint64_t base_epoch = 3;
  const uint64_t cache_epoch = package_query_cache_current_epoch();

  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store("name:contains:one", 7, base_epoch, cache_epoch, stored);
  package_query_cache_store("name:contains:two", 7, base_epoch, cache_epoch, stored);
  package_query_cache_store("name:contains:three", 7, base_epoch, cache_epoch, stored);
  package_query_cache_store("name:contains:four", 7, base_epoch, cache_epoch, stored);

  REQUIRE_FALSE(package_query_cache_lookup("name:contains:one", 7, base_epoch, cache_epoch, loaded));
  REQUIRE(package_query_cache_lookup("name:contains:two", 7, base_epoch, cache_epoch, loaded));
  REQUIRE(package_query_cache_lookup("name:contains:three", 7, base_epoch, cache_epoch, loaded));
  REQUIRE(package_query_cache_lookup("name:contains:four", 7, base_epoch, cache_epoch, loaded));
}
