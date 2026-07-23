// -----------------------------------------------------------------------------
// src/ui/package_query/package_query_cache.cpp
// Package query result cache
// Keeps cached search result storage and invalidation rules separate from the package query controller.
// -----------------------------------------------------------------------------
#include "ui/package_query/package_query_cache.hpp"

#include "dnf_backend/base_manager.hpp"

#include <cstddef>
#include <map>
#include <mutex>

namespace {

constexpr size_t kMaxSearchCacheEntries = 3;

// Cache one visible result set per search term and search option combination.
// Entries are tied to the BaseManager snapshot generation and the cache epoch.
// Snapshot generation tracks repository snapshots used by cached rows.
// The cache epoch tracks UI actions that intentionally invalidate cached search rows.
struct CachedSearchResults {
  uint64_t snapshot_generation;
  uint64_t cache_epoch;
  uint64_t last_used;
  std::vector<PackageRow> packages;
};

struct SearchCacheState {
  std::mutex mutex;
  std::map<std::string, CachedSearchResults> entries;
  uint64_t use_counter = 0;
  uint64_t epoch = 0;
};

static SearchCacheState g_search_cache;

// -----------------------------------------------------------------------------
// Drop the oldest cached search rows when the cache reaches its entry limit.
// -----------------------------------------------------------------------------
static void
package_query_cache_prune_locked()
{
  while (g_search_cache.entries.size() > kMaxSearchCacheEntries) {
    auto oldest = g_search_cache.entries.begin();
    for (auto it = g_search_cache.entries.begin(); it != g_search_cache.entries.end(); ++it) {
      if (it->second.last_used < oldest->second.last_used) {
        oldest = it;
      }
    }
    g_search_cache.entries.erase(oldest);
  }
}

}

// -----------------------------------------------------------------------------
// Build a unique cache key from search options and the search term.
// -----------------------------------------------------------------------------
std::string
package_query_cache_key_for(const std::string &term, const DnfBackendSearchOptions &options)
{
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
  std::lock_guard<std::mutex> lock(g_search_cache.mutex);
  g_search_cache.entries.clear();
  g_search_cache.use_counter = 0;
  g_search_cache.epoch++;
}

// -----------------------------------------------------------------------------
// Release cached backend metadata without dropping lightweight search rows.
// A later Base load advances the snapshot generation, which rejects rows from the old snapshot.
// -----------------------------------------------------------------------------
void
package_query_cache_drop_cached_base()
{
  BaseManager::instance().drop_cached_base();
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
// Reuse only results produced from the current snapshot generation and cache epoch.
// -----------------------------------------------------------------------------
bool
package_query_cache_lookup(const std::string &key,
                           uint64_t snapshot_generation,
                           uint64_t cache_epoch,
                           std::vector<PackageRow> &out_packages)
{
  std::lock_guard<std::mutex> lock(g_search_cache.mutex);
  auto it = g_search_cache.entries.find(key);
  if (it == g_search_cache.entries.end()) {
    return false;
  }

  if (it->second.snapshot_generation != snapshot_generation || it->second.cache_epoch != cache_epoch) {
    g_search_cache.entries.erase(it);
    return false;
  }

  it->second.last_used = ++g_search_cache.use_counter;
  out_packages = it->second.packages;
  return true;
}

// -----------------------------------------------------------------------------
// Save rows so the same search can be shown faster next time.
// Search results are reusable while the repository snapshot and cache epoch stay the same.
// -----------------------------------------------------------------------------
void
package_query_cache_store(const std::string &key,
                          uint64_t snapshot_generation,
                          uint64_t cache_epoch,
                          const std::vector<PackageRow> &packages)
{
  std::lock_guard<std::mutex> lock(g_search_cache.mutex);
  if (cache_epoch != g_search_cache.epoch) {
    return;
  }

  g_search_cache.entries[key] =
      CachedSearchResults { snapshot_generation, cache_epoch, ++g_search_cache.use_counter, packages };
  package_query_cache_prune_locked();
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
