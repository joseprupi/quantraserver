#ifndef QUANTRA_SABR_CALIBRATE_CACHE_H
#define QUANTRA_SABR_CALIBRATE_CACHE_H

#include <memory>
#include <string>
#include <vector>

#include <ql/handle.hpp>
#include <ql/termstructures/volatility/swaption/swaptionvolstructure.hpp>

namespace quantra {

/**
 * Cached value of a calibrated SABR swaption volatility cube.
 *
 * Holding both the QuantLib handle and the extracted per-node calibration
 * artifacts means subsequent finalize calls — including rebump/theta
 * iterations within a single PriceSwaptionRequest — avoid both the per-node
 * Levenberg-Marquardt fit and the matrix reshape from sparseSabrParameters().
 */
struct SabrCalibratedCube {
    QuantLib::Handle<QuantLib::SwaptionVolatilityStructure> handle;
    // Calibrated per-node grids, row-major (nExp * nTen) in the entry's order.
    std::vector<double> alpha;
    std::vector<double> beta;
    std::vector<double> rho;
    std::vector<double> nu;
    std::vector<double> perNodeRmse;
    std::vector<double> perNodeMaxError;
    // EndCriteria::Type cast to int. See QL endcriteria.hpp Type enum.
    std::vector<int> perNodeEndCriteria;
    // The cube's own idea of forwards per node. May differ from the
    // server-computed atmForwardsFlat by rounding; we store both and let the
    // diagnostics builder pick a source (server-computed is authoritative).
    std::vector<double> calibratedForwards;
};

/**
 * Process-scoped LRU cache for calibrated SABR cubes.
 *
 * Capacity is small (default 64 entries) and lookups/inserts are guarded by
 * an internal mutex. Cache miss returns an empty optional shared_ptr; on cache
 * hit, the stored entry is touched (LRU bump).
 */
class SabrCalibrateCache {
public:
    static SabrCalibrateCache& instance();

    /// Lookup. Returns nullptr on miss.
    std::shared_ptr<const SabrCalibratedCube> tryGet(const std::string& key);

    /// Insert. Replaces any prior entry for the same key.
    void put(const std::string& key, std::shared_ptr<const SabrCalibratedCube> value);

    /// Drop all entries. Test/diagnostic hook.
    void clear();

    /// Inspect current size. Test/diagnostic hook.
    size_t size() const;

private:
    SabrCalibrateCache();
    ~SabrCalibrateCache() = default;
    SabrCalibrateCache(const SabrCalibrateCache&) = delete;
    SabrCalibrateCache& operator=(const SabrCalibrateCache&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace quantra

#endif // QUANTRA_SABR_CALIBRATE_CACHE_H
