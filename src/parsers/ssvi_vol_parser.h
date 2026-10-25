#ifndef QUANTRA_SSVI_VOL_PARSER_H
#define QUANTRA_SSVI_VOL_PARSER_H

/**
 * eSSVI equity vol surfaces from a BlackVolSpec (shape SsviParams / SsviCalibrate).
 *
 * SsviParams: the slices are taken as given. Validation (all 400): the expiries are strictly
 * increasing and after the reference date, theta / rho / psi are present and finite, every
 * violated condition of ssvi::checkArbitrage is listed (condition, slice indices, amount), and
 * the numeric scan (ssvi::defaultScan) finds min g >= 0 and min dw/dt >= 0.
 *
 * SsviCalibrate: the quotes are fitted by ssvi::calibrate through the calibrate cache
 * (ssvi_calibrate_cache.h); an infeasible fit is a 400.
 *
 * Reference forward F_ref of the surface (k = ln(K / F_ref(T))):
 *   - SsviCalibrate: the fitted forwards. The dividend curve handed to SsviBlackVol is the one
 *     implied by them, Dq(T_i) = F_i Dr(T_i) / S0 (buildImpliedDividendCurve), so the surface is
 *     exactly consistent with the k the slices were fitted in;
 *   - SsviParams with `forward` on every slice: the same, from those forwards;
 *   - SsviParams with no `forward`: F_ref = S0 Dq(T) / Dr(T) from the spec's dividend and
 *     discount curves;
 *   - SsviParams with `forward` on some slices only: 400.
 */

#include "vol_surface_parsers.h"

#include <memory>
#include <string>
#include <vector>

namespace quantra {

struct SsviParseResult {
    std::shared_ptr<const SsviVolData> data;
    std::shared_ptr<SsviBlackVol> vol;
};

bool isSsviShape(quantra::enums::VolSurfaceShape shape);

/// True when ssvi_slices, option_quotes or ssvi_fit is present on the spec.
bool hasSsviFields(const quantra::BlackVolSpec* payload);

/// Required / forbidden field combination of the two SSVI shapes (400 otherwise).
void validateSsviFields(
    const quantra::BlackVolSpec* payload,
    quantra::enums::VolSurfaceShape shape,
    const std::string& id);

SsviParseResult parseSsviVol(
    const quantra::BlackVolSpec* payload,
    quantra::enums::VolSurfaceShape shape,
    const std::string& id,
    const QuantLib::Date& ref,
    const QuantLib::Calendar& cal,
    QuantLib::BusinessDayConvention bdc,
    const QuantLib::DayCounter& dc,
    const QuoteRegistry* quotes,
    const std::map<
        std::string,
        std::shared_ptr<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>>* curves);

/// 400 listing every checkArbitrage violation, then scan min g < 0 or min dw/dt < 0.
/// `context` names what is validated (a vol id, a bump).
void validateSsviSurface(const std::vector<ssvi::Slice>& slices, const std::string& context);

/// The entry's SSVI surface with every slice's ATM vol shifted by `shift` (absolute vol units):
/// theta_i' = (sqrt(theta_i / T_i) + shift)^2 T_i, rho, psi and the reference forwards kept.
/// 400 when a shifted vol is not > 0 or the shifted surface admits arbitrage.
std::shared_ptr<QuantLib::BlackVolTermStructure> shiftedSsviVol(
    const BlackVolEntry& entry, double shift);

} // namespace quantra

#endif // QUANTRA_SSVI_VOL_PARSER_H
