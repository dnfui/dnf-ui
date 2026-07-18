// -----------------------------------------------------------------------------
// Package query cache tests
// Covers cache keys, generation checks, and cache invalidation.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/base_manager.hpp"
#include "test_utils.hpp"
#include "ui/package_query/package_query_cache.hpp"

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
  REQUIRE(package_query_cache_key_for("bash") == "name:contains:latest:bash");

  set_backend_search_options(false, true);
  REQUIRE(package_query_cache_key_for("bash") == "name:exact:latest:bash");

  set_backend_search_options(true, false);
  REQUIRE(package_query_cache_key_for("bash") == "desc:contains:latest:bash");

  set_backend_search_options(true, true);
  REQUIRE(package_query_cache_key_for("bash") == "desc:exact:latest:bash");

  set_backend_search_options(false, false, false);
  REQUIRE(package_query_cache_key_for("bash") == "name:contains:all-versions:bash");
}

// -----------------------------------------------------------------------------
// Verify that cached rows are returned only for the same key and generation.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache returns rows for matching key and generation")
{
  package_query_cache_clear();
  const uint64_t cache_epoch = package_query_cache_current_epoch();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
    make_cache_row("demo-libs-1-1.x86_64", "demo-libs"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, 7, cache_epoch, stored);

  REQUIRE(package_query_cache_lookup(key, 7, cache_epoch, loaded));
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
  const uint64_t cache_epoch = package_query_cache_current_epoch();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, 7, cache_epoch, stored);

  REQUIRE_FALSE(package_query_cache_lookup(key, 8, cache_epoch, loaded));
  REQUIRE_FALSE(package_query_cache_lookup(key, 7, cache_epoch, loaded));
}

// -----------------------------------------------------------------------------
// Verify that clearing the query cache removes stored package rows.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache clear removes stored rows")
{
  package_query_cache_clear();
  const uint64_t cache_epoch = package_query_cache_current_epoch();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, 7, cache_epoch, stored);
  package_query_cache_clear();

  REQUIRE_FALSE(package_query_cache_lookup(key, 7, cache_epoch, loaded));
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
  const uint64_t cache_epoch = package_query_cache_current_epoch();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, 7, cache_epoch, stored);
  package_query_cache_clear();

  REQUIRE_FALSE(package_query_cache_lookup(key, 7, cache_epoch, loaded));
}

// -----------------------------------------------------------------------------
// Verify that cached rows survive Base drops when package state has not changed.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache keeps rows after Base drops")
{
  package_query_cache_clear();
  BaseManager::instance().reset_for_tests();
  const uint64_t cache_epoch = package_query_cache_current_epoch();
  const uint64_t generation = BaseManager::instance().current_generation();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, generation, cache_epoch, stored);
  BaseManager::instance().ensure_system_only_initialized_if_needed();
  const uint64_t base_epoch = BaseManager::instance().current_base_epoch();
  BaseManager::instance().drop_cached_base();

  REQUIRE(BaseManager::instance().current_base_epoch() > base_epoch);
  REQUIRE(package_query_cache_lookup(key, generation, cache_epoch, loaded));
  REQUIRE(loaded.size() == 1);
  REQUIRE(loaded[0].nevra == "demo-1-1.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that a stale worker does not store rows back into a newer cache epoch.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache ignores store requests from an older cache epoch")
{
  package_query_cache_clear();
  const uint64_t stale_epoch = package_query_cache_current_epoch();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_clear();
  const uint64_t current_epoch = package_query_cache_current_epoch();
  REQUIRE(current_epoch > stale_epoch);

  package_query_cache_store(key, 7, stale_epoch, stored);

  REQUIRE_FALSE(package_query_cache_lookup(key, 7, current_epoch, loaded));
}

// -----------------------------------------------------------------------------
// Verify that cached rows cannot grow without bound across many search terms.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache evicts oldest entries")
{
  package_query_cache_clear();
  const uint64_t cache_epoch = package_query_cache_current_epoch();

  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store("name:contains:one", 7, cache_epoch, stored);
  package_query_cache_store("name:contains:two", 7, cache_epoch, stored);
  package_query_cache_store("name:contains:three", 7, cache_epoch, stored);
  package_query_cache_store("name:contains:four", 7, cache_epoch, stored);

  REQUIRE_FALSE(package_query_cache_lookup("name:contains:one", 7, cache_epoch, loaded));
  REQUIRE(package_query_cache_lookup("name:contains:two", 7, cache_epoch, loaded));
  REQUIRE(package_query_cache_lookup("name:contains:three", 7, cache_epoch, loaded));
  REQUIRE(package_query_cache_lookup("name:contains:four", 7, cache_epoch, loaded));
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
