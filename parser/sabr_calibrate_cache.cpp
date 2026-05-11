#include "sabr_calibrate_cache.h"

#include <cstdlib>
#include <iostream>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace quantra {

namespace {

constexpr size_t kSabrCalibrateCacheCapacity = 64;

// Env-gated stderr logging for cache events. Reading the env var once at
// process start avoids the per-call getenv() cost on the hot path. Mirrors
// the curve cache's QUANTRA_CURVE_CACHE_LOG pattern.
bool sabrCacheLoggingEnabled() {
    static const bool kEnabled = []() {
        const char* v = std::getenv("QUANTRA_SABR_CACHE_LOG");
        return v != nullptr && std::string(v) == "1";
    }();
    return kEnabled;
}

void logCacheEvent(const std::string& event, const std::string& key, size_t size) {
    if (!sabrCacheLoggingEnabled()) return;
    // Truncate the (160-hex-char SHA-256) key for log readability; full key
    // isn't useful in logs and bloats the line.
    const std::string keyShort = key.size() > 16 ? (key.substr(0, 16) + "...") : key;
    std::cerr << "[SabrCalibrateCache] event=" << event
              << " key=" << keyShort
              << " size=" << size
              << std::endl;
}

} // namespace

struct SabrCalibrateCache::Impl {
    mutable std::mutex mu;
    // LRU order: front = most recently used, back = eviction candidate.
    std::list<std::string> lru;
    std::unordered_map<
        std::string,
        std::pair<std::shared_ptr<const SabrCalibratedCube>, std::list<std::string>::iterator>>
        map;
};

SabrCalibrateCache::SabrCalibrateCache() : impl_(std::make_unique<Impl>()) {}

SabrCalibrateCache& SabrCalibrateCache::instance() {
    static SabrCalibrateCache singleton;
    return singleton;
}

std::shared_ptr<const SabrCalibratedCube> SabrCalibrateCache::tryGet(const std::string& key) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->map.find(key);
    if (it == impl_->map.end()) {
        logCacheEvent("MISS", key, impl_->map.size());
        return nullptr;
    }
    // Touch: move key to LRU front.
    impl_->lru.erase(it->second.second);
    impl_->lru.push_front(key);
    it->second.second = impl_->lru.begin();
    logCacheEvent("HIT", key, impl_->map.size());
    return it->second.first;
}

void SabrCalibrateCache::put(const std::string& key, std::shared_ptr<const SabrCalibratedCube> value) {
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
        logCacheEvent("PUT_REPLACE", key, impl_->map.size());
        return;
    }
    impl_->lru.push_front(key);
    impl_->map.emplace(key, std::make_pair(std::move(value), impl_->lru.begin()));
    while (impl_->map.size() > kSabrCalibrateCacheCapacity && !impl_->lru.empty()) {
        const std::string& victim = impl_->lru.back();
        impl_->map.erase(victim);
        impl_->lru.pop_back();
    }
    logCacheEvent("PUT", key, impl_->map.size());
}

void SabrCalibrateCache::clear() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->lru.clear();
    impl_->map.clear();
}

size_t SabrCalibrateCache::size() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->map.size();
}

} // namespace quantra
