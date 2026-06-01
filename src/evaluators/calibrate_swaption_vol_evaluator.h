#ifndef QUANTRA_CALIBRATE_SWAPTION_VOL_EVALUATOR_H
#define QUANTRA_CALIBRATE_SWAPTION_VOL_EVALUATOR_H

/**
 * CalibrateSwaptionVolEvaluator — pure QuantLib core for the SABR swaption
 * volatility calibration endpoint.
 *
 * The heavy lifting lives in the shared swaption-vol runtime
 * (finalizeSwaptionVolEntryForPricing in request/swaption_vol_runtime), which
 * computes per-node ATM forwards and runs the per-node SABR fit through the
 * content-keyed cube cache. This evaluator resolves the referenced surface and
 * curves from the plain registry, hands the raw entry to that helper, and
 * returns the finalized entry as plain data. Diagnostics serialization happens
 * in the mapper, not here.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * calibrate_swaption_vol_mapper.{h,cpp}. The evaluator reads market data only
 * through the plain registry fields reg.rates.curves and
 * reg.volatility.swaptionVols.
 */

#include <string>

#include "pricing_context.h"
#include "pricing_registry.h"
#include "vol_surface_parsers.h"

namespace quantra {

/// Plain inputs for one SABR calibration request. The mapper has already
/// null-guarded and extracted the surface id to calibrate plus the discount
/// and forwarding curve ids used to compute per-node ATM forwards.
struct CalibrateSwaptionVolInputs {
    std::string volId;
    std::string discountingCurveId;
    std::string forwardingCurveId;
};

/// Calibration outcome. Carries the echoed vol id plus the finalized vol
/// entry (per-node SABR params and fit diagnostics populated). The mapper
/// turns the finalized entry into the SwaptionVolDiagnostics sub-table.
struct CalibrateSwaptionVolResult {
    std::string volId;
    SwaptionVolEntry finalized;
};

class CalibrateSwaptionVolEvaluator {
public:
    CalibrateSwaptionVolResult evaluate(const CalibrateSwaptionVolInputs& inputs,
                                     const PricingRegistry& reg,
                                     const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_CALIBRATE_SWAPTION_VOL_EVALUATOR_H
