#ifndef QUANTRASERVER_CURVE_CACHE_H
#define QUANTRASERVER_CURVE_CACHE_H

#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <list>
#include <iostream>
#include <cstdlib>

#include <ql/termstructures/yieldtermstructure.hpp>

namespace quantra {

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
// Global cache singleton + config
// =============================================================================

/**
 * CurveCache - Global access point for the curve caching system.
 *
 * Configuration via environment variables:
 *   QUANTRA_CURVE_CACHE_ENABLED=1       Enable caching (default: 0)
 *   QUANTRA_CURVE_CACHE_MAX_ENTRIES=100  Max L1 entries (default: 100)
 *   QUANTRA_CURVE_CACHE_LOG=1           Log hits/misses to stdout (default: 0)
 *
 * Future L2 config:
 *   QUANTRA_REDIS_HOST=127.0.0.1
 *   QUANTRA_REDIS_PORT=6379
 *   QUANTRA_CURVE_CACHE_TTL_SECONDS=3600
 */
class CurveCache {
public:
    static CurveCache& instance() {
        static CurveCache inst;
        return inst;
    }

    bool enabled() const { return enabled_; }
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
    CurveCache() {
        // Read config from env
        const char* envEnabled = std::getenv("QUANTRA_CURVE_CACHE_ENABLED");
        enabled_ = envEnabled && std::string(envEnabled) == "1";

        const char* envLog = std::getenv("QUANTRA_CURVE_CACHE_LOG");
        logging_ = envLog && std::string(envLog) == "1";

        size_t maxEntries = 100;
        const char* envMax = std::getenv("QUANTRA_CURVE_CACHE_MAX_ENTRIES");
        if (envMax) {
            int val = std::atoi(envMax);
            if (val > 0) maxEntries = static_cast<size_t>(val);
        }

        // For now, always create InProcessCurveCache.
        // When Redis is added, check QUANTRA_REDIS_HOST and create
        // a layered backend instead.
        backend_ = std::make_unique<InProcessCurveCache>(maxEntries);

        if (enabled_) {
            std::cout << "[CurveCache] Enabled. L1 max_entries=" << maxEntries
                      << " logging=" << (logging_ ? "on" : "off") << std::endl;
        }
    }

    bool enabled_ = false;
    bool logging_ = false;
    std::unique_ptr<CurveCacheBackend> backend_;
    Stats stats_;
};

} // namespace quantra

#endif // QUANTRASERVER_CURVE_CACHE_H
