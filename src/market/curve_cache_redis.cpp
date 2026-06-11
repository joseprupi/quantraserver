// =============================================================================
// Redis-backed L2 curve cache + layered (L1 + L2) backend.
//
// This is the ONLY translation unit that pulls in redis-plus-plus, keeping the
// dependency out of curve_cache.h (which is included widely). curve_cache.h
// declares MakeCurveCacheBackend(); this file defines it.
//
// Design / hard requirements:
//   - Bit-exact reconstruction: L2 stores serialized CachedCurveData via the
//     CurveSerializer byte codec (raw doubles + interpolator), so a reconstructed
//     curve matches the bootstrapped one exactly.
//   - Resilience: Redis being unreachable / slow / erroring must NEVER fail or
//     hang a pricing request. Every Redis call is wrapped; any error is treated
//     as an L2 miss / no-op and the request falls through to bootstrap. A short
//     connect/command timeout bounds the worst case. Errors are logged
//     rate-limited (gated by the cache log flag).
//   - Thread safety: redis-plus-plus sw::redis::Redis owns an internal,
//     thread-safe connection pool; one instance is shared across worker threads.
// =============================================================================

#include "curve_cache.h"
#include "curve_serializer.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

#include <sw/redis++/redis++.h>

namespace quantra {

namespace {

long long steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Mirror CurveCache::logEvent's format so L2 events read like the L1 events.
void printEvent(const std::string& key, const std::string& event,
                const std::string& extra = "") {
    std::cout << "[CurveCache] curve="
              << " key=" << key.substr(0, 20) << "..."
              << " event=" << event;
    if (!extra.empty()) std::cout << " " << extra;
    std::cout << std::endl;
}

// =============================================================================
// RedisL2Backend - pure L2 (serialized) cache backed by Redis.
// =============================================================================
class RedisL2Backend {
public:
    explicit RedisL2Backend(const CurveCacheConfig& cfg)
        : redis_(makeRedis(cfg)),
          prefix_(cfg.key_prefix),
          ttl_(cfg.l2_ttl_seconds),
          logging_(cfg.logging) {}

    // GET + deserialize. Returns nullopt on genuine miss OR any Redis/decode
    // error (error path is logged + treated as a miss so callers fall through).
    std::optional<CachedCurveData> get(const std::string& key) {
        try {
            sw::redis::OptionalString val = redis_.get(redisKey(key));
            if (!val) {
                return std::nullopt; // genuine miss (bootstrapper logs L2_MISS)
            }
            return CurveSerializer::decode(*val);
        } catch (const std::exception& e) {
            logError(key, e.what());
            return std::nullopt;
        }
    }

    // serialize + SET with TTL. Failures are swallowed (logged) — a missed
    // write just means a future miss, never a failed request.
    void put(const std::string& key, const CachedCurveData& data) {
        try {
            std::string blob = CurveSerializer::encode(data);
            redis_.set(redisKey(key), blob, ttl_);
            put_count_.fetch_add(1, std::memory_order_relaxed);
            if (logging_) printEvent(key, "L2_PUT");
        } catch (const std::exception& e) {
            logError(key, e.what());
        }
    }

    size_t putCount() const { return put_count_.load(std::memory_order_relaxed); }

private:
    static sw::redis::Redis makeRedis(const CurveCacheConfig& cfg) {
        // ConnectionOptions parses tcp://host:port[/db]; it does NOT connect
        // (redis-plus-plus connects lazily on first command), so an unreachable
        // endpoint surfaces as a per-call timeout/error rather than here.
        sw::redis::ConnectionOptions opts(cfg.redis_url);
        opts.socket_timeout  = std::chrono::milliseconds(cfg.redis_timeout_ms);
        opts.connect_timeout = std::chrono::milliseconds(cfg.redis_timeout_ms);

        sw::redis::ConnectionPoolOptions pool;
        pool.size = cfg.redis_pool_size;
        pool.wait_timeout = std::chrono::milliseconds(cfg.redis_timeout_ms);

        return sw::redis::Redis(opts, pool);
    }

    std::string redisKey(const std::string& key) const {
        return prefix_ + "v1:" + key;
    }

    // Rate-limited error logging (gated by the cache log flag). At most one
    // L2_ERROR line per window; suppressed errors are counted and reported.
    void logError(const std::string& key, const std::string& what) {
        if (!logging_) return;
        constexpr long long kWindowMs = 5000;
        long long now = steadyNowMs();
        long long last = last_err_ms_.load(std::memory_order_relaxed);
        if (now - last >= kWindowMs &&
            last_err_ms_.compare_exchange_strong(last, now,
                                                 std::memory_order_relaxed)) {
            uint64_t sup = suppressed_.exchange(0, std::memory_order_relaxed);
            std::string extra = "err=\"" + what + "\"";
            if (sup) extra += " (suppressed " + std::to_string(sup) + " similar)";
            printEvent(key, "L2_ERROR", extra);
        } else {
            suppressed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    sw::redis::Redis redis_;
    std::string prefix_;
    std::chrono::seconds ttl_;
    bool logging_;
    std::atomic<uint64_t> put_count_{0};
    std::atomic<long long> last_err_ms_{0};
    std::atomic<uint64_t> suppressed_{0};
};

// =============================================================================
// LayeredCurveCache - composes L1 (in-process) + optional L2 (Redis).
//
// Per-level enable flags are honored here: a disabled level is a getX-miss /
// putX-no-op. With L1 on + no L2 this is behaviorally identical to a plain
// InProcessCurveCache (today's default path).
// =============================================================================
class LayeredCurveCache : public CurveCacheBackend {
public:
    LayeredCurveCache(const CurveCacheConfig& cfg,
                      std::unique_ptr<RedisL2Backend> l2)
        : l1_(cfg.max_entries),
          l1_enabled_(cfg.l1_enabled),
          l2_enabled_(cfg.l2_enabled),
          l2_(std::move(l2)) {}

    std::shared_ptr<QuantLib::YieldTermStructure>
    getL1(const std::string& key) override {
        return l1_enabled_ ? l1_.getL1(key) : nullptr;
    }

    void putL1(const std::string& key,
               std::shared_ptr<QuantLib::YieldTermStructure> curve) override {
        if (l1_enabled_) l1_.putL1(key, std::move(curve));
    }

    std::optional<CachedCurveData> getL2(const std::string& key) override {
        if (l2_enabled_ && l2_) return l2_->get(key);
        return std::nullopt;
    }

    void putL2(const std::string& key, const CachedCurveData& data) override {
        if (l2_enabled_ && l2_) l2_->put(key, data);
    }

    void clear() override { l1_.clear(); }
    size_t sizeL1() const override { return l1_enabled_ ? l1_.sizeL1() : 0; }
    size_t sizeL2() const override { return l2_ ? l2_->putCount() : 0; }

private:
    InProcessCurveCache l1_;
    bool l1_enabled_;
    bool l2_enabled_;
    std::unique_ptr<RedisL2Backend> l2_;
};

} // namespace

// =============================================================================
// Factory (declared in curve_cache.h)
// =============================================================================
std::unique_ptr<CurveCacheBackend> MakeCurveCacheBackend(const CurveCacheConfig& cfg) {
    std::unique_ptr<RedisL2Backend> l2;

    if (cfg.l2_enabled && !cfg.redis_url.empty()) {
        try {
            l2 = std::make_unique<RedisL2Backend>(cfg);
        } catch (const std::exception& e) {
            // Bad URL / option parse failure: degrade to L2 no-op rather than
            // failing startup. Pricing still works via L1 + bootstrap.
            std::cerr << "[CurveCache] Failed to init Redis L2 ('" << cfg.redis_url
                      << "'): " << e.what()
                      << "; L2 disabled (requests fall back to bootstrap)." << std::endl;
            l2.reset();
        }
    }

    return std::make_unique<LayeredCurveCache>(cfg, std::move(l2));
}

} // namespace quantra
