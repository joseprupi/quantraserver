#ifndef QUANTRA_SWAPTION_VOL_DIAGNOSTICS_H
#define QUANTRA_SWAPTION_VOL_DIAGNOSTICS_H

#include <string>
#include <vector>

#include <flatbuffers/flatbuffers.h>

#include "diagnostics_generated.h"
#include "vol_surface_parsers.h"

namespace quantra {

/**
 * Build a SwaptionVolDiagnostics block for a fully-finalized swaption vol
 * surface entry.
 *
 * Populates the surface-level fields (expiries, tenors, forward_per_node,
 * atm_vol_per_node, time_to_expiry_per_node, alpha/beta/rho/nu_per_node) for
 * any SABR-kind surface that has been through finalize. For SabrCalibrate
 * surfaces it additionally populates the calibration sub-block (per-node
 * RMSE/max error, convergence flag, per-strike fit errors, strike spreads).
 *
 * For SabrParams surfaces the calibration sub-block is omitted (null on the
 * resulting table), matching the schema's optional-sub-block contract.
 *
 * `extraWarnings` is appended to any internally-detected warnings (e.g. large
 * fit error). Pass {} when there's nothing to forward.
 *
 * The helper is intentionally request-handler-agnostic: it takes only an
 * entry + an id + builder, so it can be reused by /sample-vol-surfaces,
 * /price-swaption, and Step 9's /calibrate-swaption-vol without duplication.
 */
flatbuffers::Offset<quantra::SwaptionVolDiagnostics> buildSwaptionVolDiagnostics(
    flatbuffers::FlatBufferBuilder& fbb,
    const std::string& volId,
    const SwaptionVolEntry& entry,
    const std::vector<std::string>& extraWarnings = {});

/**
 * Build a partial SwaptionVolDiagnostics block carrying only vol_id, kind,
 * and a populated warnings vector. Used when the surface failed to construct
 * but the caller wants to surface the failure rather than abort the entire
 * response.
 */
flatbuffers::Offset<quantra::SwaptionVolDiagnostics> buildPartialSwaptionVolDiagnostics(
    flatbuffers::FlatBufferBuilder& fbb,
    const std::string& volId,
    quantra::enums::SwaptionVolKind kind,
    const std::vector<std::string>& warnings);

} // namespace quantra

#endif // QUANTRA_SWAPTION_VOL_DIAGNOSTICS_H
