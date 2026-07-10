#include "hw_calibrate_cache.h"

#include <cstdlib>
#include <iostream>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "swaption_model_calibration.h"

namespace quantra {

namespace {

constexpr size_t kHwCalibrateCacheCapacity = 64;

// Env-gated stderr logging for cache events. Reading the env var once at
// process start avoids the per-call getenv() cost on the hot path. Mirrors the
// SABR/curve caches' QUANTRA_*_CACHE_LOG pattern.
bool hwCacheLoggingEnabled() {
    static const bool kEnabled = []() {
        const char* v = std::getenv("QUANTRA_HW_CACHE_LOG");
        return v != nullptr && std::string(v) == "1";
    }();
    return kEnabled;
}

void logCacheEvent(const std::string& event, const std::string& key, size_t size) {
    if (!hwCacheLoggingEnabled()) return;
    // Truncate the (SHA-256) key for log readability; the full key isn't useful
    // in logs and bloats the line.
    const std::string keyShort = key.size() > 16 ? (key.substr(0, 16) + "...") : key;
    std::cerr << "[HwCalibCache] event=" << event
              << " key=" << keyShort
              << " size=" << size
              << std::endl;
}

} // namespace

struct HwCalibrateCache::Impl {
    mutable std::mutex mu;
    // LRU order: front = most recently used, back = eviction candidate.
    std::list<std::string> lru;
    std::unordered_map<
        std::string,
        std::pair<std::shared_ptr<const HwCalibResult>, std::list<std::string>::iterator>>
        map;
};

HwCalibrateCache::HwCalibrateCache() : impl_(std::make_unique<Impl>()) {}

HwCalibrateCache& HwCalibrateCache::instance() {
    static HwCalibrateCache singleton;
    return singleton;
}

namespace {

// Test-only override. std::nullopt = use the env-derived value below. Written
// only by tests (single-threaded); never set in production, so the hot path
// reads it as a plain unset optional.
std::optional<bool>& hwEnabledOverride() {
    static std::optional<bool> ov;
    return ov;
}

} // namespace

bool HwCalibrateCache::enabled() {
    if (const auto& ov = hwEnabledOverride()) {
        return *ov;
    }
    // Read the env flag once at first call (default off). Mirrors how the SABR
    // cache reads QUANTRA_SABR_CACHE_ENABLED, avoiding per-request getenv() on
    // the hot path.
    static const bool kEnabled = []() {
        const char* v = std::getenv("QUANTRA_HW_CACHE_ENABLED");
        return v != nullptr && std::string(v) == "1";
    }();
    return kEnabled;
}

void HwCalibrateCache::setEnabledOverrideForTesting(std::optional<bool> override) {
    hwEnabledOverride() = override;
}

std::optional<bool> HwCalibrateCache::enabledOverrideForTesting() {
    return hwEnabledOverride();
}

std::shared_ptr<const HwCalibResult> HwCalibrateCache::tryGet(const std::string& key) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->map.find(key);
    if (it == impl_->map.end()) {
        logCacheEvent("miss", key, impl_->map.size());
        return nullptr;
    }
    // Touch: move key to LRU front.
    impl_->lru.erase(it->second.second);
    impl_->lru.push_front(key);
    it->second.second = impl_->lru.begin();
    logCacheEvent("hit", key, impl_->map.size());
    return it->second.first;
}

void HwCalibrateCache::put(const std::string& key, std::shared_ptr<const HwCalibResult> value) {
    if (!value) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->map.find(key);
    if (it != impl_->map.end()) {
        impl_->lru.erase(it->second.second);
        impl_->lru.push_front(key);
        it->second.first = std::move(value);
        it->second.second = impl_->lru.begin();
        logCacheEvent("put_replace", key, impl_->map.size());
        return;
    }
    impl_->lru.push_front(key);
    impl_->map.emplace(key, std::make_pair(std::move(value), impl_->lru.begin()));
    while (impl_->map.size() > kHwCalibrateCacheCapacity && !impl_->lru.empty()) {
        const std::string& victim = impl_->lru.back();
        impl_->map.erase(victim);
        impl_->lru.pop_back();
    }
    logCacheEvent("put", key, impl_->map.size());
}

void HwCalibrateCache::clear() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->lru.clear();
    impl_->map.clear();
}

size_t HwCalibrateCache::size() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->map.size();
}

} // namespace quantra
