#ifndef QUANTRA_SSVI_VOL_PARSER_H
#define QUANTRA_SSVI_VOL_PARSER_H

/**
 * eSSVI equity vol surfaces from an `SsviVolSpec` payload (src/market/ssvi_vol_surface.h).
 *
 * The slices are taken as given. Validation (all 400): base conventions present, base shape /
 * constant_vol / quote_id absent, spot present, finite and > 0, at least one slice, expiries
 * strictly increasing and after the reference date, forward / theta / rho / psi present and
 * finite, forward > 0, strike_min / strike_max finite and > 0 with strike_min < strike_max, every
 * violated condition of ssvi::checkArbitrage listed (condition, slice indices, amount), and the
 * numeric scan (ssvi::defaultScan) finds min g >= 0 and min dw/dt >= 0.
 *
 * Reference forward F_ref (k = ln(K / F_ref(T))): ln F_ref linear in time through ln spot at the
 * reference date and ln forward at every slice (makeSsviSurface), so the payload needs no curve.
 *
 * The entry's handle is the SsviBlackVol; a structured note derives the closed-form SSVI local
 * vol from it (mc::LocalVolCache recognises the type, src/mc/basket_model.h) instead of
 * QuantLib's finite-difference LocalVolSurface.
 */

#include "ssvi.h"
#include "vol_surface_parsers.h"

#include <memory>
#include <string>
#include <vector>

namespace quantra {

/// 400 listing every checkArbitrage violation, then scan min g < 0 or min dw/dt < 0.
/// `context` names what is validated (a vol id, a bump).
void validateSsviSurface(const std::vector<ssvi::Slice>& slices, const std::string& context);

/// Parse an SsviVolSpec payload into a BlackVolEntry (shapeFb SsviSlices). Strike ranges and
/// local vol time extrapolation as documented on the payload.
BlackVolEntry parseSsviVol(const quantra::VolSurfaceSpec* spec);

/// The entry's SSVI surface with every slice's ATM vol shifted by `shift` (absolute vol units):
/// theta_i' = (sqrt(theta_i / T_i) + shift)^2 T_i, rho, psi and the reference forwards kept
/// (the vega bump of a structured note). 400 when a shifted vol is not > 0 or the shifted
/// surface admits arbitrage.
std::shared_ptr<QuantLib::BlackVolTermStructure> shiftedSsviVol(
    const BlackVolEntry& entry, double shift);

} // namespace quantra

#endif // QUANTRA_SSVI_VOL_PARSER_H
