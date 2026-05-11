#include "sabr_calibrate_cache.h"

#include <list>
#include <mutex>
#include <unordered_map>

namespace quantra {

namespace {

constexpr size_t kSabrCalibrateCacheCapacity = 64;

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
        return nullptr;
    }
    // Touch: move key to LRU front.
    impl_->lru.erase(it->second.second);
    impl_->lru.push_front(key);
    it->second.second = impl_->lru.begin();
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
        return;
    }
    impl_->lru.push_front(key);
    impl_->map.emplace(key, std::make_pair(std::move(value), impl_->lru.begin()));
    while (impl_->map.size() > kSabrCalibrateCacheCapacity && !impl_->lru.empty()) {
        const std::string& victim = impl_->lru.back();
        impl_->map.erase(victim);
        impl_->lru.pop_back();
    }
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
