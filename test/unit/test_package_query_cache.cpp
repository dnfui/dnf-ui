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

  REQUIRE(package_query_cache_key_for("bash", backend_search_options(false, false)) == "name:contains:latest:bash");

  REQUIRE(package_query_cache_key_for("bash", backend_search_options(false, true)) == "name:exact:latest:bash");

  REQUIRE(package_query_cache_key_for("bash", backend_search_options(true, false)) == "desc:contains:latest:bash");

  REQUIRE(package_query_cache_key_for("bash", backend_search_options(true, true)) == "desc:exact:latest:bash");

  REQUIRE(package_query_cache_key_for("bash", backend_search_options(false, false, false)) == "name:contains:all:bash");

  auto provider_options = backend_search_options(false, false);
  provider_options.find_provider = true;
  REQUIRE(package_query_cache_key_for("bash", provider_options) == "provides:contains:latest:bash");
}

// -----------------------------------------------------------------------------
// Verify that only compact latest-only searches use the in-memory search cache.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache is used only for latest-only searches")
{
  REQUIRE(package_query_cache_should_use(backend_search_options(false, false)));
  REQUIRE_FALSE(package_query_cache_should_use(backend_search_options(false, false, false)));
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
  package_query_cache_store("provides:contains:latest:demo", 7, cache_epoch, stored);
  package_query_cache_clear();

  REQUIRE_FALSE(package_query_cache_lookup(key, 7, cache_epoch, loaded));
  REQUIRE_FALSE(package_query_cache_lookup("provides:contains:latest:demo", 7, cache_epoch, loaded));
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
// Verify that cached search snapshots survive shared Base memory release.
// -----------------------------------------------------------------------------
TEST_CASE("Search cache survives shared Base memory release")
{
  package_query_cache_clear();
  auto &manager = BaseManager::instance();
  manager.reset_for_tests();
  manager.initialize_system_only_base_for_tests();
  REQUIRE(manager.has_cached_base_for_tests());

  const uint64_t cache_epoch = package_query_cache_current_epoch();
  const uint64_t generation = manager.current_generation();

  const std::string key = "name:contains:demo";
  std::vector<PackageRow> stored = {
    make_cache_row("demo-1-1.x86_64", "demo"),
  };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, generation, cache_epoch, stored);
  manager.drop_cached_base();

  REQUIRE_FALSE(manager.has_cached_base_for_tests());
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
// Verify that reading a cached entry keeps it from being the next entry evicted.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache lookup updates entry recency")
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

  REQUIRE(package_query_cache_lookup("name:contains:one", 7, cache_epoch, loaded));

  package_query_cache_store("name:contains:four", 7, cache_epoch, stored);

  REQUIRE(package_query_cache_lookup("name:contains:one", 7, cache_epoch, loaded));
  REQUIRE_FALSE(package_query_cache_lookup("name:contains:two", 7, cache_epoch, loaded));
  REQUIRE(package_query_cache_lookup("name:contains:three", 7, cache_epoch, loaded));
  REQUIRE(package_query_cache_lookup("name:contains:four", 7, cache_epoch, loaded));
}

// -----------------------------------------------------------------------------
// Each search mode must retain its own three entries when the other mode fills its cache.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache modes have independent capacity")
{
  package_query_cache_clear();
  const uint64_t epoch = package_query_cache_current_epoch();
  const auto packages = backend_search_options(false, false);
  auto providers = packages;
  providers.find_provider = true;
  const std::vector<PackageRow> rows = { make_cache_row("demo-1-1.x86_64", "demo") };
  std::vector<PackageRow> loaded;

  for (const std::string term : { "one", "two", "three" }) {
    package_query_cache_store(package_query_cache_key_for(term, packages), 7, epoch, rows);
  }
  for (const std::string term : { "one", "two", "three", "four" }) {
    package_query_cache_store(package_query_cache_key_for(term, providers), 7, epoch, rows);
  }
  for (const std::string term : { "one", "two", "three" }) {
    REQUIRE(package_query_cache_lookup(package_query_cache_key_for(term, packages), 7, epoch, loaded));
  }
  REQUIRE_FALSE(package_query_cache_lookup(package_query_cache_key_for("one", providers), 7, epoch, loaded));

  for (const std::string term : { "five", "six", "seven", "eight" }) {
    package_query_cache_store(package_query_cache_key_for(term, packages), 7, epoch, rows);
  }
  for (const std::string term : { "two", "three", "four" }) {
    REQUIRE(package_query_cache_lookup(package_query_cache_key_for(term, providers), 7, epoch, loaded));
  }
  REQUIRE_FALSE(package_query_cache_lookup(package_query_cache_key_for("five", packages), 7, epoch, loaded));
}

// -----------------------------------------------------------------------------
// Repository changes must invalidate provider rows and reject results from earlier searches.
// -----------------------------------------------------------------------------
TEST_CASE("Package query cache validates provider generation and epoch")
{
  package_query_cache_clear();
  const uint64_t epoch = package_query_cache_current_epoch();
  const std::string key = "provides:contains:latest:demo";
  const std::vector<PackageRow> rows = { make_cache_row("demo-1-1.x86_64", "demo") };
  std::vector<PackageRow> loaded;

  package_query_cache_store(key, 7, epoch, rows);
  REQUIRE_FALSE(package_query_cache_lookup(key, 8, epoch, loaded));
  REQUIRE_FALSE(package_query_cache_lookup(key, 7, epoch, loaded));

  package_query_cache_clear();
  const uint64_t new_epoch = package_query_cache_current_epoch();
  package_query_cache_store(key, 7, epoch, rows);
  REQUIRE_FALSE(package_query_cache_lookup(key, 7, new_epoch, loaded));
  package_query_cache_store(key, 7, new_epoch, rows);
  REQUIRE(package_query_cache_lookup(key, 7, new_epoch, loaded));
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
