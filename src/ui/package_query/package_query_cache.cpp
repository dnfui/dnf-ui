// -----------------------------------------------------------------------------
// src/ui/package_query/package_query_cache.cpp
// Package query result cache
// Keeps cached search result storage and invalidation rules separate from the package query controller.
// -----------------------------------------------------------------------------
#include "ui/package_query/package_query_cache.hpp"

#include <cstddef>
#include <map>
#include <mutex>

namespace {

constexpr size_t kMaxSearchCacheEntries = 3;

// Cache one visible result set per search term and search option combination.
// Entries are tied to the BaseManager generation and the cache epoch.
// Generation tracks backend rebuilds.
// The cache epoch tracks UI actions that intentionally invalidate cached search rows.
struct CachedSearchResults {
  uint64_t generation;
  uint64_t cache_epoch;
  uint64_t last_used;
  std::vector<PackageRow> packages;
};

using SearchCacheEntries = std::map<std::string, CachedSearchResults>;

struct SearchCacheState {
  std::mutex mutex;
  SearchCacheEntries package_entries;
  SearchCacheEntries provider_entries;
  uint64_t use_counter = 0;
  uint64_t epoch = 0;
};

static SearchCacheState g_search_cache;

// -----------------------------------------------------------------------------
// Select storage by the search mode encoded in the cache key.
// Each mode retains its own entry limit so Provides cannot evict ordinary searches.
// -----------------------------------------------------------------------------
static SearchCacheEntries &
package_query_cache_entries_locked(const std::string &key)
{
  if (key.starts_with("provides:")) {
    return g_search_cache.provider_entries;
  }
  return g_search_cache.package_entries;
}

// -----------------------------------------------------------------------------
// Drop the oldest cached search rows when the cache reaches its entry limit.
// -----------------------------------------------------------------------------
static void
package_query_cache_prune_locked(SearchCacheEntries &entries)
{
  while (entries.size() > kMaxSearchCacheEntries) {
    auto oldest = entries.begin();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
      if (it->second.last_used < oldest->second.last_used) {
        oldest = it;
      }
    }
    entries.erase(oldest);
  }
}

}

// -----------------------------------------------------------------------------
// Build a unique cache key from search options and the search term.
// -----------------------------------------------------------------------------
std::string
package_query_cache_key_for(const std::string &term, const DnfBackendSearchOptions &options)
{
  std::string key = options.find_provider ? "provides:" : (options.search_in_description ? "desc:" : "name:");
  key += (options.exact_match ? "exact:" : "contains:");
  key += (options.latest_only ? "latest:" : "all:");
  key += term;

  return key;
}

// -----------------------------------------------------------------------------
// Cache only the normal compact search view.
// All-version searches can be much larger than the current table.
// -----------------------------------------------------------------------------
bool
package_query_cache_should_use(const DnfBackendSearchOptions &options)
{
  return options.latest_only;
}

// -----------------------------------------------------------------------------
// Clear cached search results.
// Used by the Clear Cache button, repository refresh, transaction refresh, and installed-state refresh.
// Advancing the cache epoch also prevents older searches from storing rows back into a cache state the UI already
// invalidated.
// -----------------------------------------------------------------------------
void
package_query_cache_clear()
{
  std::lock_guard<std::mutex> lock(g_search_cache.mutex);
  g_search_cache.package_entries.clear();
  g_search_cache.provider_entries.clear();
  g_search_cache.use_counter = 0;
  g_search_cache.epoch++;
}

// -----------------------------------------------------------------------------
// Return the current cache epoch.
// -----------------------------------------------------------------------------
uint64_t
package_query_cache_current_epoch()
{
  std::lock_guard<std::mutex> lock(g_search_cache.mutex);
  return g_search_cache.epoch;
}

// -----------------------------------------------------------------------------
// Look up cached rows before starting a new backend query.
// Reuse only results produced from the current Base generation and cache epoch.
// Base drops are a memory choice and do not make package rows stale by themselves.
// -----------------------------------------------------------------------------
bool
package_query_cache_lookup(const std::string &key,
                           uint64_t generation,
                           uint64_t cache_epoch,
                           std::vector<PackageRow> &out_packages)
{
  std::lock_guard<std::mutex> lock(g_search_cache.mutex);
  auto &entries = package_query_cache_entries_locked(key);
  auto it = entries.find(key);
  if (it == entries.end()) {
    return false;
  }

  if (it->second.generation != generation || it->second.cache_epoch != cache_epoch) {
    entries.erase(it);
    return false;
  }

  it->second.last_used = ++g_search_cache.use_counter;
  out_packages = it->second.packages;
  return true;
}

// -----------------------------------------------------------------------------
// Save rows so the same search can be shown faster next time.
// Search results are reusable while package state and the cache epoch stay the same.
// -----------------------------------------------------------------------------
void
package_query_cache_store(const std::string &key,
                          uint64_t generation,
                          uint64_t cache_epoch,
                          const std::vector<PackageRow> &packages)
{
  std::lock_guard<std::mutex> lock(g_search_cache.mutex);
  if (cache_epoch != g_search_cache.epoch) {
    return;
  }

  auto &entries = package_query_cache_entries_locked(key);
  entries[key] = CachedSearchResults { generation, cache_epoch, ++g_search_cache.use_counter, packages };
  package_query_cache_prune_locked(entries);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
