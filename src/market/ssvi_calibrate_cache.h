#ifndef QUANTRA_SSVI_CALIBRATE_CACHE_H
#define QUANTRA_SSVI_CALIBRATE_CACHE_H

#include "ssvi_calibrator.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace quantra {

/**
 * Content-keyed SHA-256 key of an eSSVI calibration: everything calibrate() reads. The curve
 * inputs enter through the values the calibrator actually consumes (the risk-free and dividend
 * discount factors at every quote expiry time), so two requests whose curves agree at the
 * expiries share an entry whatever their curve ids are.
 *
 *   - reference date, day-counter name, spot, weighting, max |log-moneyness|
 *   - per quote: expiry, strike, type, price, implied vol, bid, ask, weight (the optionals
 *     with a presence flag)
 *   - a flag for the presence of a dividend curve, then per distinct expiry time: D_r(T) and,
 *     with a dividend curve, D_q(T)
 *
 * Output: "ssvi-calib:v1:<sha256hex>".
 */
std::string buildSsviCalibrateCacheKey(const ssvi::CalibrationInput& input);

/**
 * Process-scoped LRU cache of calibration results (capacity 64, mutex guarded), mirroring
 * SabrCalibrateCache.
 *
 *   QUANTRA_SSVI_CACHE_ENABLED=1   Enable store/reuse (default: 0 = off)
 *   QUANTRA_SSVI_CACHE_LOG=1       Log hit/miss events to stderr (default: 0)
 */
class SsviCalibrateCache {
public:
    static SsviCalibrateCache& instance();

    static bool enabled();
    /// Test-only: force enabled() regardless of the env flag; std::nullopt restores it.
    static void setEnabledOverrideForTesting(std::optional<bool> override);

    std::shared_ptr<const ssvi::CalibrationResult> tryGet(const std::string& key);
    void put(const std::string& key, std::shared_ptr<const ssvi::CalibrationResult> value);
    void clear();
    std::size_t size() const;
    /// Hits since process start (or the last clear()). Test/diagnostic hook.
    std::size_t hits() const;

private:
    SsviCalibrateCache();
    ~SsviCalibrateCache() = default;
    SsviCalibrateCache(const SsviCalibrateCache&) = delete;
    SsviCalibrateCache& operator=(const SsviCalibrateCache&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// ssvi::calibrate through the cache when it is enabled (always a fresh fit when disabled).
std::shared_ptr<const ssvi::CalibrationResult> calibrateCached(const ssvi::CalibrationInput& input);

} // namespace quantra

#endif // QUANTRA_SSVI_CALIBRATE_CACHE_H
