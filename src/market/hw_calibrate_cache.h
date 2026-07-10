#ifndef QUANTRA_HW_CALIBRATE_CACHE_H
#define QUANTRA_HW_CALIBRATE_CACHE_H

#include <memory>
#include <optional>
#include <string>

namespace quantra {

// Defined in swaption_model_calibration.h. Forward-declared here so this
// header stays light; only shared_ptr-to-const is needed in the interface.
struct HwCalibResult;

/**
 * Process-scoped LRU cache for calibrated Hull-White model parameters.
 *
 * A single swaption-model calibration runs a Levenberg-Marquardt fit over the
 * whole expiry/tenor grid (~150ms for the 24-helper example). The result is a
 * deterministic function of the consumed market vols, the resolved grid, the
 * discount/forwarding curves, the swap-index conventions and the calibration
 * spec — all captured by hw_calibrate_cache_key. Caching that result lets
 * repeated identical requests (same market snapshot) skip the fit entirely.
 *
 * Capacity is small (default 64 entries) and lookups/inserts are guarded by an
 * internal mutex. Cache miss returns a null shared_ptr; on cache hit the stored
 * entry is touched (LRU bump).
 *
 * Configuration via environment variables (mirrors the SABR/curve caches):
 *   QUANTRA_HW_CACHE_ENABLED=1   Enable store/reuse (default: 0 = off)
 *   QUANTRA_HW_CACHE_LOG=1       Log hit/miss events to stderr (default: 0)
 *
 * When disabled, callers must skip tryGet/put and calibrate fresh every
 * request; the switch only controls whether results are cached/reused.
 *
 * Eviction is LRU only (no TTL): the key embeds the as-of date and the full set
 * of consumed inputs, so a cached entry can never be stale — only unused.
 */
class HwCalibrateCache {
public:
    static HwCalibrateCache& instance();

    /// Whether HW-calibrate caching is enabled. Reads QUANTRA_HW_CACHE_ENABLED
    /// once at first call (default off) and caches it in a static const for the
    /// hot path. Mirrors SabrCalibrateCache::enabled(). A test override, when
    /// set, takes precedence.
    static bool enabled();

    /// Test-only: force enabled() to a fixed value regardless of the env flag.
    /// std::nullopt restores the normal env-derived behavior.
    static void setEnabledOverrideForTesting(std::optional<bool> override);
    static std::optional<bool> enabledOverrideForTesting();

    /// Lookup. Returns nullptr on miss.
    std::shared_ptr<const HwCalibResult> tryGet(const std::string& key);

    /// Insert. Replaces any prior entry for the same key.
    void put(const std::string& key, std::shared_ptr<const HwCalibResult> value);

    /// Drop all entries. Test/diagnostic hook.
    void clear();

    /// Inspect current size. Test/diagnostic hook.
    size_t size() const;

private:
    HwCalibrateCache();
    ~HwCalibrateCache() = default;
    HwCalibrateCache(const HwCalibrateCache&) = delete;
    HwCalibrateCache& operator=(const HwCalibrateCache&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace quantra

#endif // QUANTRA_HW_CALIBRATE_CACHE_H
