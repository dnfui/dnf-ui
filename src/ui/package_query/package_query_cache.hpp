// -----------------------------------------------------------------------------
// src/ui/package_query/package_query_cache.hpp
// Package query result cache
//
// Owns cached search result storage so the package query controller does not
// need to manage cache keys, generations, or locking directly.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <cstdint>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Build the cache key from the current search options and search term.
// -----------------------------------------------------------------------------
std::string package_query_cache_key_for(const std::string &term);
// -----------------------------------------------------------------------------
// Clear all cached package query results.
// -----------------------------------------------------------------------------
void package_query_cache_clear();
// -----------------------------------------------------------------------------
// Return the current search-cache invalidation epoch.
// The epoch advances when UI actions explicitly invalidate cached search rows
// without requiring a backend Base rebuild.
// -----------------------------------------------------------------------------
uint64_t package_query_cache_current_epoch();
// -----------------------------------------------------------------------------
// Look up cached package rows for one key, generation, and cache epoch.
// -----------------------------------------------------------------------------
bool package_query_cache_lookup(const std::string &key,
                                uint64_t generation,
                                uint64_t cache_epoch,
                                std::vector<PackageRow> &out_packages);
// -----------------------------------------------------------------------------
// Store package rows for one key, generation, and cache epoch.
// -----------------------------------------------------------------------------
void package_query_cache_store(const std::string &key,
                               uint64_t generation,
                               uint64_t cache_epoch,
                               const std::vector<PackageRow> &packages);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
