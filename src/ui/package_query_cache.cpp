// -----------------------------------------------------------------------------
// src/ui/package_query_cache.cpp
// Package query result cache
// Keeps cached search result storage and invalidation rules separate from the package query controller.
// -----------------------------------------------------------------------------
#include "package_query_cache.hpp"

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

static std::map<std::string, CachedSearchResults> g_search_cache;
static std::mutex g_cache_mutex; // Protects g_search_cache
static uint64_t g_cache_use_counter = 0;
static uint64_t g_cache_epoch = 0;

// -----------------------------------------------------------------------------
// Drop the oldest cached search rows when the cache reaches its entry limit.
// -----------------------------------------------------------------------------
static void
package_query_cache_prune_locked()
{
  while (g_search_cache.size() > kMaxSearchCacheEntries) {
    auto oldest = g_search_cache.begin();
    for (auto it = g_search_cache.begin(); it != g_search_cache.end(); ++it) {
      if (it->second.last_used < oldest->second.last_used) {
        oldest = it;
      }
    }
    g_search_cache.erase(oldest);
  }
}

}

// -----------------------------------------------------------------------------
// Build a unique cache key from search options and the search term.
// -----------------------------------------------------------------------------
std::string
package_query_cache_key_for(const std::string &term)
{
  const DnfBackendSearchOptions options = dnf_backend_get_search_options();
  std::string key = (options.search_in_description ? "desc:" : "name:");
  key += (options.exact_match ? "exact:" : "contains:");
  key += term;

  return key;
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
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  g_search_cache.clear();
  g_cache_use_counter = 0;
  g_cache_epoch++;
}

// -----------------------------------------------------------------------------
// Return the current cache epoch.
// -----------------------------------------------------------------------------
uint64_t
package_query_cache_current_epoch()
{
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  return g_cache_epoch;
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
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  auto it = g_search_cache.find(key);
  if (it == g_search_cache.end()) {
    return false;
  }

  if (it->second.generation != generation || it->second.cache_epoch != cache_epoch) {
    g_search_cache.erase(it);
    return false;
  }

  it->second.last_used = ++g_cache_use_counter;
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
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  if (cache_epoch != g_cache_epoch) {
    return;
  }

  g_search_cache[key] = CachedSearchResults { generation, cache_epoch, ++g_cache_use_counter, packages };
  package_query_cache_prune_locked();
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
