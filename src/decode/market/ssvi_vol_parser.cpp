#include "ssvi_vol_parser.h"

#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"
#include "roll_offset.h"
#include "ssvi_calibrator.h"
#include "ssvi_vol_surface.h"

#include <cmath>
#include <limits>
#include <sstream>

namespace quantra {

namespace {

std::string forId(const std::string& id) {
    return " for vol id: " + id;
}

double requireValue(
    const flatbuffers::Optional<double>& v,
    const std::string& path,
    const char* field,
    const std::string& id) {
    if (!v.has_value()) {
        QUANTRA_INVALID_ARGUMENT(path + "." + field + " is required" + forId(id));
    }
    if (!std::isfinite(v.value())) {
        QUANTRA_INVALID_ARGUMENT(path + "." + field + " must be finite" + forId(id));
    }
    return v.value();
}

} // namespace

void validateSsviSurface(const std::vector<ssvi::Slice>& slices, const std::string& context) {
    const ssvi::Surface surface(slices); // 400 naming the slice for invalid parameters
    const std::vector<ssvi::Violation> violations = ssvi::checkArbitrage(slices);
    if (!violations.empty()) {
        std::ostringstream os;
        os << "SSVI surface admits arbitrage (" << context << "):";
        for (const ssvi::Violation& v : violations) {
            os << " " << v.condition << " at slice " << v.slice;
            if (v.prevSlice != v.slice) os << " vs slice " << v.prevSlice;
            os << " (violated by " << v.margin << ");";
        }
        QUANTRA_INVALID_ARGUMENT(os.str());
    }
    const ssvi::ScanResult scan = ssvi::defaultScan(surface);
    if (!(scan.minG >= 0.0) || !(scan.minWt >= 0.0)) {
        std::ostringstream os;
        os << "SSVI surface admits arbitrage (" << context
           << "): numeric scan finds min g = " << scan.minG << " at (t=" << scan.minGt
           << ", k=" << scan.minGk << ") and min dw/dt = " << scan.minWt << " at (t=" << scan.minWtT
           << ", k=" << scan.minWtK << ")";
        QUANTRA_INVALID_ARGUMENT(os.str());
    }
}

BlackVolEntry parseSsviVol(const quantra::VolSurfaceSpec* spec) {
    if (!spec || !spec->id()) {
        QUANTRA_INVALID_ARGUMENT("VolSurfaceSpec or id is null");
    }
    const std::string id = spec->id()->str();
    const auto* payload = spec->payload_as_SsviVolSpec();
    if (!payload) {
        QUANTRA_INVALID_ARGUMENT("SsviVolSpec payload missing" + forId(id));
    }
    const auto* b = payload->base();
    if (!b) {
        QUANTRA_INVALID_ARGUMENT("SsviVolSpec.base is required" + forId(id));
    }
    if (!b->reference_date() || b->reference_date()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("SsviVolSpec.base.reference_date is required" + forId(id));
    }
    if (!b->calendar().has_value()) {
        QUANTRA_INVALID_ARGUMENT("SsviVolSpec.base.calendar is required" + forId(id));
    }
    if (!b->business_day_convention().has_value()) {
        QUANTRA_INVALID_ARGUMENT(
            "SsviVolSpec.base.business_day_convention is required" + forId(id));
    }
    if (!b->day_counter().has_value()) {
        QUANTRA_INVALID_ARGUMENT("SsviVolSpec.base.day_counter is required" + forId(id));
    }
    if (b->shape() != quantra::enums::VolSurfaceShape_Constant) {
        QUANTRA_INVALID_ARGUMENT(
            "SsviVolSpec.base.shape must be absent (the payload is the shape)" + forId(id));
    }
    if (b->constant_vol() != 0.0 || (b->quote_id() && b->quote_id()->size() > 0)) {
        QUANTRA_INVALID_ARGUMENT(
            "SsviVolSpec.base.constant_vol / quote_id are not part of an SSVI payload" + forId(id));
    }
    const double spot = requireValue(payload->spot(), "SsviVolSpec", "spot", id);
    if (!(spot > 0.0)) {
        QUANTRA_INVALID_ARGUMENT("SsviVolSpec.spot must be > 0" + forId(id));
    }
    if (!payload->slices() || payload->slices()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("SsviVolSpec.slices must contain at least one slice" + forId(id));
    }
    const QuantLib::Date ref = applyRollOffset(DateToQL(b->reference_date()->str()));
    const QuantLib::Calendar cal = CalendarToQL(b->calendar().value());
    const QuantLib::BusinessDayConvention bdc =
        ConventionToQL(b->business_day_convention().value());
    const QuantLib::DayCounter dc = DayCounterToQL(b->day_counter().value());

    std::vector<QuantLib::Date> dates;
    std::vector<SsviSliceParams> params;
    std::vector<double> forwards;
    std::vector<ssvi::Slice> slices;
    double surfaceMinStrike = QL_MAX_REAL, surfaceMaxStrike = 0.0;
    for (flatbuffers::uoffset_t i = 0; i < payload->slices()->size(); ++i) {
        const auto* sl = payload->slices()->Get(i);
        const std::string path = "SsviVolSpec.slices[" + std::to_string(i) + "]";
        if (!sl || !sl->expiry() || sl->expiry()->size() == 0) {
            QUANTRA_INVALID_ARGUMENT(path + ".expiry is required" + forId(id));
        }
        const QuantLib::Date d = DateToQL(sl->expiry()->str());
        if (d <= ref) {
            QUANTRA_INVALID_ARGUMENT(path + ".expiry must be after reference_date" + forId(id));
        }
        if (!dates.empty() && d <= dates.back()) {
            QUANTRA_INVALID_ARGUMENT(
                path + ".expiry: slices must be strictly increasing" + forId(id));
        }
        const double forward = requireValue(sl->forward(), path, "forward", id);
        const double theta = requireValue(sl->theta(), path, "theta", id);
        const double rho = requireValue(sl->rho(), path, "rho", id);
        const double psi = requireValue(sl->psi(), path, "psi", id);
        if (!(forward > 0.0)) {
            QUANTRA_INVALID_ARGUMENT(path + ".forward must be > 0" + forId(id));
        }
        if (!(theta > 0.0)) {
            QUANTRA_INVALID_ARGUMENT(path + ".theta must be > 0" + forId(id));
        }
        if (!(std::fabs(rho) < 1.0)) {
            QUANTRA_INVALID_ARGUMENT(path + ".rho must be in (-1, 1)" + forId(id));
        }
        if (!(psi > 0.0)) {
            QUANTRA_INVALID_ARGUMENT(path + ".psi must be > 0" + forId(id));
        }
        const auto kMin = sl->strike_min();
        const auto kMax = sl->strike_max();
        for (const auto& [v, name] :
             {std::make_pair(kMin, ".strike_min"), std::make_pair(kMax, ".strike_max")}) {
            if (v.has_value() && !(std::isfinite(*v) && *v > 0.0)) {
                QUANTRA_INVALID_ARGUMENT(path + name + " must be a finite strike > 0" + forId(id));
            }
        }
        if (kMin.has_value() && kMax.has_value() && !(*kMin < *kMax)) {
            QUANTRA_INVALID_ARGUMENT(path + ".strike_min must be below strike_max" + forId(id));
        }
        surfaceMinStrike = std::min(surfaceMinStrike, kMin.has_value() ? *kMin : 0.0);
        surfaceMaxStrike = std::max(surfaceMaxStrike, kMax.has_value() ? *kMax : QL_MAX_REAL);
        dates.push_back(d);
        forwards.push_back(forward);
        params.push_back(SsviSliceParams{theta, rho, psi});
        slices.push_back(ssvi::Slice{dc.yearFraction(ref, d), theta, rho, psi});
    }
    validateSsviSurface(slices, "vol id " + id);

    BlackVolEntry entry;
    entry.handle = QuantLib::Handle<QuantLib::BlackVolTermStructure>(
        makeSsviSurface(ref, cal, bdc, dc, dates, params, forwards, spot));
    if (payload->local_vol_strike_range().value_or(quantra::enums::LocalVolStrikeRange_Global) ==
        quantra::enums::LocalVolStrikeRange_PerExpiry) {
        for (flatbuffers::uoffset_t i = 0; i < payload->slices()->size(); ++i) {
            const auto* sl = payload->slices()->Get(i);
            if (!sl->strike_min().has_value() || !sl->strike_max().has_value()) {
                QUANTRA_INVALID_ARGUMENT(
                    "SsviVolSpec.slices[" + std::to_string(i) +
                    "]: strike_min and strike_max are required with local_vol_strike_range "
                    "PerExpiry" +
                    forId(id));
            }
            entry.localVolStrikeBands.push_back({dates[i], *sl->strike_min(), *sl->strike_max()});
        }
    } else if (
        payload->local_vol_strike_range().has_value() &&
        *payload->local_vol_strike_range() != quantra::enums::LocalVolStrikeRange_Global) {
        QUANTRA_INVALID_ARGUMENT("SsviVolSpec.local_vol_strike_range is not known" + forId(id));
    }
    if (surfaceMinStrike > 0.0 || surfaceMaxStrike < QL_MAX_REAL) {
        entry.localVolStrikeRange = std::make_pair(surfaceMinStrike, surfaceMaxStrike);
    }
    if (payload->local_vol_time_extrapolation().has_value()) {
        switch (*payload->local_vol_time_extrapolation()) {
            case quantra::enums::LocalVolTimeExtrapolation_FlatLocalVol:
                entry.flatLocalVolBeyondLastExpiry = true;
                break;
            case quantra::enums::LocalVolTimeExtrapolation_FlatImpliedVol:
                QUANTRA_INVALID_ARGUMENT(
                    "SsviVolSpec.local_vol_time_extrapolation: FlatImpliedVol is not an eSSVI "
                    "reading (beyond the last slice theta continues the last forward variance); "
                    "leave it absent for the model's own continuation or set FlatLocalVol" +
                    forId(id));
            default:
                QUANTRA_INVALID_ARGUMENT(
                    "SsviVolSpec.local_vol_time_extrapolation is not known" + forId(id));
        }
    }
    entry.constantVol = std::numeric_limits<double>::quiet_NaN();
    entry.shapeFb = quantra::enums::VolSurfaceShape_SsviSlices;
    entry.referenceDate = ref;
    entry.calendar = cal;
    entry.calendarFb = b->calendar().value();
    entry.businessDayConvention = bdc;
    entry.businessDayConventionFb = b->business_day_convention().value();
    entry.dayCounter = dc;
    entry.pillarDates = dates;
    // Defined at every time and strike: nothing to extrapolate.
    entry.allowExtrapolation = true;
    entry.handle->enableExtrapolation();
    return entry;
}

std::shared_ptr<QuantLib::BlackVolTermStructure> shiftedSsviVol(
    const BlackVolEntry& entry, double shift) {
    const auto base = std::dynamic_pointer_cast<SsviBlackVol>(entry.handle.currentLink());
    if (!base) {
        QUANTRA_ERROR("Black vol entry has no SSVI surface to shift");
    }
    const std::vector<ssvi::Slice>& sl = base->surface().slices();
    std::vector<SsviSliceParams> params;
    std::vector<ssvi::Slice> slices;
    for (std::size_t i = 0; i < sl.size(); ++i) {
        const double t = sl[i].t;
        const double vol = std::sqrt(sl[i].theta / t) + shift;
        if (!(vol > 0.0)) {
            std::ostringstream os;
            os << "Black vol shift " << shift << " makes the SSVI ATM vol of slice " << i
               << " non-positive (" << vol << ")";
            QUANTRA_INVALID_ARGUMENT(os.str());
        }
        params.push_back(SsviSliceParams{vol * vol * t, sl[i].rho, sl[i].psi});
        slices.push_back(ssvi::Slice{t, vol * vol * t, sl[i].rho, sl[i].psi});
    }
    std::ostringstream ctx;
    ctx << "vol bump " << shift;
    validateSsviSurface(slices, ctx.str());
    auto shifted = std::make_shared<SsviBlackVol>(
        base->referenceDate(), base->calendar(), base->businessDayConvention(), base->dayCounter(),
        base->expiries(), params, base->spot0(), base->dividend(), base->riskFree());
    shifted->enableExtrapolation();
    return shifted;
}

} // namespace quantra
