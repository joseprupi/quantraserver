#ifndef QUANTRASERVER_CURVE_CACHE_H
#define QUANTRASERVER_CURVE_CACHE_H

#include <string>
#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <list>
#include <iostream>
#include <cstdlib>
#include <cctype>
#include <limits>

#include <ql/termstructures/yieldtermstructure.hpp>

namespace quantra {

namespace detail {

inline std::optional<size_t> ParsePositiveSizeT(const char* text) {
    if (text == nullptr) {
        return std::nullopt;
    }

    std::string value(text);
    auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (begin >= end) {
        return std::nullopt;
    }

    value = std::string(begin, end);
    size_t parsed_chars = 0;
    unsigned long long parsed = 0;
    try {
        parsed = std::stoull(value, &parsed_chars, 10);
    } catch (const std::exception&) {
        return std::nullopt;
    }

    if (parsed_chars != value.size() || parsed == 0 ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        return std::nullopt;
    }

    return static_cast<size_t>(parsed);
}

} // namespace detail

// =============================================================================
// Serialized curve representation (for L2 / Redis)
// =============================================================================

/**
 * CachedCurveData - Portable representation of a bootstrapped yield curve.
 *
 * Contains pillar dates + discount factors + metadata needed to reconstruct
 * an equivalent InterpolatedDiscountCurve.
 */
struct CachedCurveData {
    std::string reference_date;     // YYYY-MM-DD
    uint8_t     day_counter;        // enums::DayCounter
    uint8_t     interpolator;       // enums::Interpolator
    std::vector<std::string> dates; // pillar dates YYYY-MM-DD
    std::vector<double> discount_factors; // aligned with dates
};

// =============================================================================
// Abstract cache interface
// =============================================================================

/**
 * CurveCacheBackend - Abstract interface for curve cache storage.
 *
 * L1 (in-process) stores live QuantLib objects.
 * L2 (Redis, future) stores serialized CachedCurveData.
 *
 * The layered cache checks L1 first, then L2 if available.
 */
class CurveCacheBackend {
public:
    virtual ~CurveCacheBackend() = default;

    // --- L1: Live QuantLib object cache ---
    // Returns nullptr on cache miss; callers must handle misses explicitly.
    virtual std::shared_ptr<QuantLib::YieldTermStructure>
        getL1(const std::string& key) = 0;

    virtual void putL1(
        const std::string& key,
        std::shared_ptr<QuantLib::YieldTermStructure> curve) = 0;

    // --- L2: Serialized cache (for cross-process sharing) ---
    // Returns nullopt if not implemented or not found
    virtual std::optional<CachedCurveData>
        getL2(const std::string& key) = 0;

    virtual void putL2(
        const std::string& key,
        const CachedCurveData& data) = 0;

    // --- Management ---
    virtual void clear() = 0;
    virtual size_t sizeL1() const = 0;
    virtual size_t sizeL2() const = 0;
};


// =============================================================================
// L1-only in-process LRU cache
// =============================================================================

/**
 * InProcessCurveCache - LRU cache of live QuantLib YieldTermStructure objects.
 *
 * Single-threaded (no locking needed — each worker is a single-threaded process).
 * L2 methods return nullopt / no-op (ready for override by Redis backend).
 */
class InProcessCurveCache : public CurveCacheBackend {
public:
    explicit InProcessCurveCache(size_t maxEntries = 100)
        : maxEntries_(maxEntries) {}

    // Returns nullptr on miss and updates LRU state on hit.
    std::shared_ptr<QuantLib::YieldTermStructure>
    getL1(const std::string& key) override {
        auto it = cacheMap_.find(key);
        if (it == cacheMap_.end()) return nullptr;

        // Move to front (most recently used)
        lruList_.splice(lruList_.begin(), lruList_, it->second.lruIt);
        return it->second.curve;
    }

    void putL1(
        const std::string& key,
        std::shared_ptr<QuantLib::YieldTermStructure> curve) override
    {
        auto it = cacheMap_.find(key);
        if (it != cacheMap_.end()) {
            // Update existing
            it->second.curve = curve;
            lruList_.splice(lruList_.begin(), lruList_, it->second.lruIt);
            return;
        }

        // Evict if at capacity
        while (cacheMap_.size() >= maxEntries_ && !lruList_.empty()) {
            auto& evictKey = lruList_.back();
            cacheMap_.erase(evictKey);
            lruList_.pop_back();
        }

        // Insert
        lruList_.push_front(key);
        cacheMap_[key] = { curve, lruList_.begin() };
    }

    // L2 not implemented — return nullopt / no-op
    std::optional<CachedCurveData> getL2(const std::string& /*key*/) override {
        return std::nullopt;
    }

    void putL2(const std::string& /*key*/, const CachedCurveData& /*data*/) override {
        // No-op for L1-only cache
    }

    void clear() override {
        cacheMap_.clear();
        lruList_.clear();
    }

    size_t sizeL1() const override { return cacheMap_.size(); }
    size_t sizeL2() const override { return 0; }

private:
    struct Entry {
        std::shared_ptr<QuantLib::YieldTermStructure> curve;
        std::list<std::string>::iterator lruIt;
    };

    size_t maxEntries_;
    std::unordered_map<std::string, Entry> cacheMap_;
    std::list<std::string> lruList_; // front = most recent
};


// =============================================================================
// Cache configuration + backend factory
// =============================================================================

/**
 * CurveCacheConfig - Resolved configuration for the curve caching system.
 * Populated once at startup from environment variables (see CurveCache ctor).
 */
struct CurveCacheConfig {
    bool        l1_enabled = false;  // in-process LRU of live QL curves
    bool        l2_enabled = false;  // Redis-backed serialized cache
    size_t      max_entries = 100;   // L1 capacity
    bool        logging = false;

    // L2 / Redis
    std::string redis_url;                       // empty => L2 stays no-op
    std::string key_prefix = "quantra:curvecache:";
    uint32_t    l2_ttl_seconds = 14400;          // 4h default
    size_t      redis_pool_size = 8;
    int         redis_timeout_ms = 200;          // connect + command timeout
};

/**
 * Build the cache backend for the given config.
 *
 * Defined in curve_cache_redis.cpp (the only TU that pulls in redis-plus-plus):
 *   - L2 enabled with a redis_url -> layered cache (InProcess L1 + Redis L2)
 *   - otherwise                   -> plain InProcessCurveCache (today's behavior)
 *
 * Per-level enable gating (l1_enabled / l2_enabled) is honored inside the
 * returned backend, so a disabled level becomes a getX-miss / putX-no-op.
 */
std::unique_ptr<CurveCacheBackend> MakeCurveCacheBackend(const CurveCacheConfig& cfg);


// =============================================================================
// Global cache singleton + config
// =============================================================================

/**
 * CurveCache - Global access point for the curve caching system.
 *
 * Configuration via environment variables (read once at startup):
 *   QUANTRA_CURVE_CACHE_L1_ENABLED=1     Enable L1 in-process cache (default: 0)
 *   QUANTRA_CURVE_CACHE_L2_ENABLED=1     Enable L2 Redis cache      (default: 0)
 *   QUANTRA_CURVE_CACHE_ENABLED=1        Backward-compat alias for L1 (default: 0)
 *   QUANTRA_CURVE_CACHE_MAX_ENTRIES=100  Max L1 entries (default: 100)
 *   QUANTRA_CURVE_CACHE_LOG=1            Log hits/misses to stdout (default: 0)
 *
 *   QUANTRA_REDIS_URL=tcp://127.0.0.1:6379       L2 endpoint (empty => L2 no-op)
 *   QUANTRA_CURVE_CACHE_L2_KEY_PREFIX=quantra:curvecache:
 *   QUANTRA_CURVE_CACHE_L2_TTL_SECONDS=14400     L2 entry TTL (default: 4h)
 *   QUANTRA_REDIS_POOL_SIZE=8                     L2 connection pool size
 *   QUANTRA_REDIS_TIMEOUT_MS=200                  L2 connect/command timeout
 */
class CurveCache {
public:
    static CurveCache& instance() {
        static CurveCache inst;
        return inst;
    }

    // enabled() == "any caching level active" — gates the cache code path.
    bool enabled() const { return l1_enabled_ || l2_enabled_; }
    bool l1Enabled() const { return l1_enabled_; }
    bool l2Enabled() const { return l2_enabled_; }
    bool logging() const { return logging_; }

    CurveCacheBackend& backend() { return *backend_; }

    // --- Stats ---
    struct Stats {
        uint64_t l1_hits = 0;
        uint64_t l1_misses = 0;
        uint64_t l2_hits = 0;
        uint64_t l2_misses = 0;
        uint64_t bootstraps = 0;
    };

    Stats& stats() { return stats_; }
    const Stats& stats() const { return stats_; }

    void resetStats() { stats_ = Stats{}; }

    void logEvent(const std::string& curveId, const std::string& key,
                  const std::string& event, double timeMs = 0.0) const {
        if (!logging_) return;
        std::cout << "[CurveCache] curve=" << curveId
                  << " key=" << key.substr(0, 20) << "..."
                  << " event=" << event;
        if (timeMs > 0.0) std::cout << " time=" << timeMs << "ms";
        std::cout << std::endl;
    }

private:
    static bool envFlag(const char* name) {
        const char* v = std::getenv(name);
        return v && std::string(v) == "1";
    }

    CurveCache() {
        CurveCacheConfig cfg;

        // L1: explicit flag OR backward-compat alias QUANTRA_CURVE_CACHE_ENABLED.
        cfg.l1_enabled = envFlag("QUANTRA_CURVE_CACHE_L1_ENABLED") ||
                         envFlag("QUANTRA_CURVE_CACHE_ENABLED");
        cfg.l2_enabled = envFlag("QUANTRA_CURVE_CACHE_L2_ENABLED");
        cfg.logging    = envFlag("QUANTRA_CURVE_CACHE_LOG");

        const char* envMax = std::getenv("QUANTRA_CURVE_CACHE_MAX_ENTRIES");
        if (envMax) {
            if (auto parsed = detail::ParsePositiveSizeT(envMax)) {
                cfg.max_entries = *parsed;
            } else {
                std::cerr << "[CurveCache] Invalid QUANTRA_CURVE_CACHE_MAX_ENTRIES='"
                          << envMax << "'; using default " << cfg.max_entries << std::endl;
            }
        }

        // --- L2 / Redis config ---
        if (const char* url = std::getenv("QUANTRA_REDIS_URL")) {
            cfg.redis_url = url;
        }
        if (const char* prefix = std::getenv("QUANTRA_CURVE_CACHE_L2_KEY_PREFIX")) {
            if (*prefix) cfg.key_prefix = prefix;
        }
        if (const char* ttl = std::getenv("QUANTRA_CURVE_CACHE_L2_TTL_SECONDS")) {
            if (auto parsed = detail::ParsePositiveSizeT(ttl)) {
                cfg.l2_ttl_seconds = static_cast<uint32_t>(*parsed);
            } else {
                std::cerr << "[CurveCache] Invalid QUANTRA_CURVE_CACHE_L2_TTL_SECONDS='"
                          << ttl << "'; using default " << cfg.l2_ttl_seconds << std::endl;
            }
        }
        if (const char* poolSize = std::getenv("QUANTRA_REDIS_POOL_SIZE")) {
            if (auto parsed = detail::ParsePositiveSizeT(poolSize)) {
                cfg.redis_pool_size = *parsed;
            }
        }
        if (const char* timeout = std::getenv("QUANTRA_REDIS_TIMEOUT_MS")) {
            if (auto parsed = detail::ParsePositiveSizeT(timeout)) {
                cfg.redis_timeout_ms = static_cast<int>(*parsed);
            }
        }

        l1_enabled_ = cfg.l1_enabled;
        l2_enabled_ = cfg.l2_enabled;
        logging_    = cfg.logging;

        backend_ = MakeCurveCacheBackend(cfg);

        if (enabled_()) {
            std::cout << "[CurveCache] Enabled."
                      << " L1=" << (l1_enabled_ ? "on" : "off")
                      << " L2=" << (l2_enabled_ ? "on" : "off")
                      << " max_entries=" << cfg.max_entries;
            if (l2_enabled_) {
                std::cout << " redis_url=" << (cfg.redis_url.empty() ? "(unset)" : cfg.redis_url)
                          << " key_prefix=" << cfg.key_prefix
                          << " ttl=" << cfg.l2_ttl_seconds << "s";
            }
            std::cout << " logging=" << (logging_ ? "on" : "off") << std::endl;
        }
    }

    bool enabled_() const { return l1_enabled_ || l2_enabled_; }

    bool l1_enabled_ = false;
    bool l2_enabled_ = false;
    bool logging_ = false;
    std::unique_ptr<CurveCacheBackend> backend_;
    Stats stats_;
};

} // namespace quantra

#endif // QUANTRASERVER_CURVE_CACHE_H
