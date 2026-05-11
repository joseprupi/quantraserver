#ifndef QUANTRA_SABR_CALIBRATE_CACHE_KEY_H
#define QUANTRA_SABR_CALIBRATE_CACHE_KEY_H

#include <string>
#include <vector>

#include "vol_surface_parsers.h"

namespace quantra {

/**
 * Build a content-keyed SHA-256 cache key for a constructed
 * XabrSwaptionVolatilityCube<SwaptionVolCubeSabrModel> instance.
 *
 * The key captures everything that affects calibration output, mirroring the
 * approach in curve_cache_key.cpp:
 *   - expiries, tenors, strike spreads (rate units)
 *   - market vol cube (resolved, post-quote-registry)
 *   - calibration options: beta_fixed, beta_value, vega_weighted_smile_fit
 *   - swap_index_id
 *   - discount-curve cache-key + forwarding-curve cache-key
 *   - reference date, displacement, volatility type
 *   - server-computed ATM forwards (small but matters for cube identity)
 *
 * `discountCurveKey` and `forwardingCurveKey` are the cache keys returned by
 * the curve bootstrapper for the curves passed into finalize. They carry the
 * full curve identity (helpers + quotes + dependencies); using them keeps the
 * SABR key small and avoids re-serializing curve internals here.
 *
 * Output: "sabr-cube:v1:<sha256hex>".
 */
std::string buildSabrCalibrateCacheKey(
    const SwaptionVolEntry& finalizedInputs,
    const std::vector<double>& atmForwardsFlat,
    const std::string& discountCurveKey,
    const std::string& forwardingCurveKey);

} // namespace quantra

#endif // QUANTRA_SABR_CALIBRATE_CACHE_KEY_H
