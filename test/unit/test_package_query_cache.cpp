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

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
    make_cache_row("demo-libs-1-1.x86_64", "demo-libs"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, 7, stored);

  REQUIRE(package_query_cache_lookup(key, 7, loaded));
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

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, 7, stored);

  REQUIRE_FALSE(package_query_cache_lookup(key, 8, loaded));
  REQUIRE_FALSE(package_query_cache_lookup(key, 7, loaded));
}

// -----------------------------------------------------------------------------
// Verify that clearing the query cache removes stored package rows.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache clear removes stored rows")
{
  package_query_cache_clear();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, 7, stored);
  package_query_cache_clear();

  REQUIRE_FALSE(package_query_cache_lookup(key, 7, loaded));
}

// -----------------------------------------------------------------------------
// Verify that cached rows cannot grow without bound across many search terms.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache evicts oldest entries")
{
  package_query_cache_clear();

  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store("name:contains:one", 7, stored);
  package_query_cache_store("name:contains:two", 7, stored);
  package_query_cache_store("name:contains:three", 7, stored);
  package_query_cache_store("name:contains:four", 7, stored);

  REQUIRE_FALSE(package_query_cache_lookup("name:contains:one", 7, loaded));
  REQUIRE(package_query_cache_lookup("name:contains:two", 7, loaded));
  REQUIRE(package_query_cache_lookup("name:contains:three", 7, loaded));
  REQUIRE(package_query_cache_lookup("name:contains:four", 7, loaded));
}
