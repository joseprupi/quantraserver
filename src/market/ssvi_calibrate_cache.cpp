#include "ssvi_calibrate_cache.h"

#include "curve_cache_key.h"

#include <openssl/sha.h>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <list>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace quantra {

namespace {

constexpr std::size_t kSsviCalibrateCacheCapacity = 64;

bool ssviCacheLoggingEnabled() {
    static const bool kEnabled = []() {
        const char* v = std::getenv("QUANTRA_SSVI_CACHE_LOG");
        return v != nullptr && std::string(v) == "1";
    }();
    return kEnabled;
}

void logCacheEvent(const std::string& event, const std::string& key, std::size_t size) {
    if (!ssviCacheLoggingEnabled()) return;
    const std::string keyShort = key.size() > 24 ? (key.substr(0, 24) + "...") : key;
    std::cerr << "[SsviCalibrateCache] event=" << event << " key=" << keyShort << " size=" << size
              << std::endl;
}

std::optional<bool>& enabledOverride() {
    static std::optional<bool> ov;
    return ov;
}

std::string sha256hex(const std::vector<uint8_t>& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

void writeOptional(CanonicalBuffer& buf, const std::optional<double>& v) {
    buf.writeBool(v.has_value());
    buf.writeDouble(v.value_or(0.0));
}

} // namespace

std::string buildSsviCalibrateCacheKey(const ssvi::CalibrationInput& in) {
    CanonicalBuffer buf;
    buf.writeTag("ssvi-calib:v1:");
    buf.writeI32(static_cast<int32_t>(in.referenceDate.serialNumber()));
    buf.writeString(in.dayCounter.name());
    buf.writeDouble(in.spot);
    buf.writeU8(static_cast<uint8_t>(in.weighting));
    buf.writeDouble(in.maxAbsLogMoneyness);

    buf.writeU32(static_cast<uint32_t>(in.quotes.size()));
    std::set<QuantLib::Date> expiries;
    for (const ssvi::OptionQuote& q : in.quotes) {
        expiries.insert(q.expiry);
        buf.writeI32(static_cast<int32_t>(q.expiry.serialNumber()));
        buf.writeDouble(q.strike);
        buf.writeU8(static_cast<uint8_t>(q.type));
        writeOptional(buf, q.price);
        writeOptional(buf, q.impliedVol);
        writeOptional(buf, q.bid);
        writeOptional(buf, q.ask);
        buf.writeDouble(q.weight);
    }

    // The only curve inputs calibrate() reads: D_r and (when present) D_q at the expiry times.
    buf.writeU8(in.dividend.empty() ? 0 : 1);
    buf.writeU32(static_cast<uint32_t>(expiries.size()));
    for (const QuantLib::Date& d : expiries) {
        const double t = in.dayCounter.yearFraction(in.referenceDate, d);
        buf.writeDouble(t);
        buf.writeDouble(in.discount->discount(t, true));
        if (!in.dividend.empty()) buf.writeDouble(in.dividend->discount(t, true));
    }
    appendCalendarOverridesSection(buf);
    return "ssvi-calib:v1:" + sha256hex(buf.data());
}

struct SsviCalibrateCache::Impl {
    mutable std::mutex mu;
    std::list<std::string> lru; // front = most recently used
    std::unordered_map<
        std::string,
        std::pair<std::shared_ptr<const ssvi::CalibrationResult>, std::list<std::string>::iterator>>
        map;
    std::size_t hits = 0;
};

SsviCalibrateCache::SsviCalibrateCache() : impl_(std::make_unique<Impl>()) {}

SsviCalibrateCache& SsviCalibrateCache::instance() {
    static SsviCalibrateCache singleton;
    return singleton;
}

bool SsviCalibrateCache::enabled() {
    if (const auto& ov = enabledOverride()) return *ov;
    static const bool kEnabled = []() {
        const char* v = std::getenv("QUANTRA_SSVI_CACHE_ENABLED");
        return v != nullptr && std::string(v) == "1";
    }();
    return kEnabled;
}

void SsviCalibrateCache::setEnabledOverrideForTesting(std::optional<bool> override) {
    enabledOverride() = override;
}

std::shared_ptr<const ssvi::CalibrationResult> SsviCalibrateCache::tryGet(const std::string& key) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->map.find(key);
    if (it == impl_->map.end()) {
        logCacheEvent("MISS", key, impl_->map.size());
        return nullptr;
    }
    impl_->lru.erase(it->second.second);
    impl_->lru.push_front(key);
    it->second.second = impl_->lru.begin();
    ++impl_->hits;
    logCacheEvent("HIT", key, impl_->map.size());
    return it->second.first;
}

void SsviCalibrateCache::put(
    const std::string& key, std::shared_ptr<const ssvi::CalibrationResult> value) {
    if (!value) return;
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
    while (impl_->map.size() > kSsviCalibrateCacheCapacity && !impl_->lru.empty()) {
        impl_->map.erase(impl_->lru.back());
        impl_->lru.pop_back();
    }
    logCacheEvent("PUT", key, impl_->map.size());
}

void SsviCalibrateCache::clear() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->lru.clear();
    impl_->map.clear();
    impl_->hits = 0;
}

std::size_t SsviCalibrateCache::size() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->map.size();
}

std::size_t SsviCalibrateCache::hits() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->hits;
}

std::shared_ptr<const ssvi::CalibrationResult> calibrateCached(
    const ssvi::CalibrationInput& input) {
    if (!SsviCalibrateCache::enabled()) {
        return std::make_shared<const ssvi::CalibrationResult>(ssvi::calibrate(input));
    }
    ssvi::validateInput(input); // before the key reads the curves
    const std::string key = buildSsviCalibrateCacheKey(input);
    auto& cache = SsviCalibrateCache::instance();
    if (auto hit = cache.tryGet(key)) return hit;
    auto fresh = std::make_shared<const ssvi::CalibrationResult>(ssvi::calibrate(input));
    cache.put(key, fresh);
    return fresh;
}

} // namespace quantra
