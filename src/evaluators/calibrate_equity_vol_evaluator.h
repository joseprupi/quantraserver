#ifndef QUANTRA_CALIBRATE_EQUITY_VOL_EVALUATOR_H
#define QUANTRA_CALIBRATE_EQUITY_VOL_EVALUATOR_H

/**
 * CalibrateEquityVolEvaluator — pure core of the equity eSSVI calibration endpoint.
 *
 * The fit itself runs when the registry parses the SsviCalibrate BlackVolSpec
 * (parseBlackVol -> ssvi::calibrate through the calibrate cache), so that the surface is
 * usable by every pricing endpoint. This evaluator resolves the requested surface from the plain
 * registry, rejects any other shape, and returns the SSVI data (slices, forwards, quotes, fit
 * diagnostics) as plain data. The mapper turns it into the wire response.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or mention
 * FlatBuffers/gRPC namespaces (scripts/check_evaluator_boundary.sh). All wire-format conversion
 * lives in calibrate_equity_vol_mapper.{h,cpp}. The evaluator reads market data only through
 * reg.volatility.blackVols.
 */

#include "pricing_context.h"
#include "pricing_registry.h"
#include "vol_surface_parsers.h"

#include <memory>
#include <string>

namespace quantra {

struct CalibrateEquityVolInputs {
    std::string volId;
};

struct CalibrateEquityVolResult {
    std::string volId;
    std::shared_ptr<const SsviVolData> data; // data->fit is set
};

class CalibrateEquityVolEvaluator {
public:
    CalibrateEquityVolResult evaluate(
        const CalibrateEquityVolInputs& inputs,
        const PricingRegistry& reg,
        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_CALIBRATE_EQUITY_VOL_EVALUATOR_H
