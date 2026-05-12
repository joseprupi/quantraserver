#include "sample_vol_surfaces_request.h"

#include <sstream>
#include <limits>
#include <tuple>
#include <algorithm>
#include <cctype>
#include <cmath>

#include "common_parser.h"
#include "error.h"
#include "swaption_vol_runtime.h"
#include "swaption_vol_diagnostics.h"

using namespace QuantLib;
using namespace quantra;

namespace {

struct GridConventions {
    Calendar qlCalendar;
    enums::Calendar fbCalendar;
    BusinessDayConvention qlBdc;
    enums::BusinessDayConvention fbBdc;
};

enums::TimeUnit qlTimeUnitToFb(QuantLib::TimeUnit u) {
    switch (u) {
        case QuantLib::Days: return enums::TimeUnit_Days;
        case QuantLib::Weeks: return enums::TimeUnit_Weeks;
        case QuantLib::Months: return enums::TimeUnit_Months;
        case QuantLib::Years: return enums::TimeUnit_Years;
        default: return enums::TimeUnit_Days;
    }
}

GridConventions resolveGridConventions(
    const DateGridSpec* gridSpec,
    const QueryOptions* options,
    const Calendar& fallbackCalendar,
    enums::Calendar fallbackFbCalendar,
    BusinessDayConvention fallbackBdc,
    enums::BusinessDayConvention fallbackFbBdc) {
    GridConventions gc;
    gc.qlCalendar = fallbackCalendar;
    gc.fbCalendar = fallbackFbCalendar;
    gc.qlBdc = fallbackBdc;
    gc.fbBdc = fallbackFbBdc;
    auto shouldUseOverrideBdc = [](enums::BusinessDayConvention bdc, enums::Calendar cal) {
        // QueryOptions/TenorGrid/RangeGrid default to Following even when omitted.
        // Treat Following as "no explicit override" unless calendar is explicitly set.
        return cal != enums::Calendar_NullCalendar || bdc != enums::BusinessDayConvention_Following;
    };

    if (options && options->calendar() != enums::Calendar_NullCalendar) {
        gc.qlCalendar = CalendarToQL(options->calendar());
        gc.fbCalendar = options->calendar();
    } else if (gridSpec && gridSpec->grid_type() == DateGrid_TenorGrid) {
        auto grid = gridSpec->grid_as_TenorGrid();
        if (grid->calendar() != enums::Calendar_NullCalendar) {
            gc.qlCalendar = CalendarToQL(grid->calendar());
            gc.fbCalendar = grid->calendar();
        }
    } else if (gridSpec && gridSpec->grid_type() == DateGrid_RangeGrid) {
        auto grid = gridSpec->grid_as_RangeGrid();
        if (grid->calendar() != enums::Calendar_NullCalendar) {
            gc.qlCalendar = CalendarToQL(grid->calendar());
            gc.fbCalendar = grid->calendar();
        }
    }
    if (options && shouldUseOverrideBdc(options->business_day_convention(), options->calendar())) {
        gc.qlBdc = ConventionToQL(options->business_day_convention());
        gc.fbBdc = options->business_day_convention();
        return gc;
    }
    if (!gridSpec) return gc;
    if (gridSpec->grid_type() == DateGrid_TenorGrid) {
        const auto* g = gridSpec->grid_as_TenorGrid();
        if (shouldUseOverrideBdc(g->business_day_convention(), g->calendar())) {
            gc.qlBdc = ConventionToQL(g->business_day_convention());
            gc.fbBdc = g->business_day_convention();
        }
        return gc;
    }
    if (gridSpec->grid_type() == DateGrid_RangeGrid) {
        const auto* g = gridSpec->grid_as_RangeGrid();
        if (shouldUseOverrideBdc(g->business_day_convention(), g->calendar())) {
            gc.qlBdc = ConventionToQL(g->business_day_convention());
            gc.fbBdc = g->business_day_convention();
        }
        return gc;
    }
    return gc;
}

std::vector<Date> buildDateGrid(
    const DateGridSpec* gridSpec,
    const Date& referenceDate,
    const Date& asOfDate,
    const QueryOptions* options,
    const Calendar& fallbackCalendar,
    enums::Calendar fallbackFbCalendar,
    BusinessDayConvention fallbackBdc,
    enums::BusinessDayConvention fallbackFbBdc) {
    if (!gridSpec) {
        QUANTRA_INVALID_ARGUMENT("DateGridSpec is required");
    }
    std::vector<Date> dates;
    GridConventions gc = resolveGridConventions(
        gridSpec, options, fallbackCalendar, fallbackFbCalendar, fallbackBdc, fallbackFbBdc);
    Calendar calendar = gc.qlCalendar;
    BusinessDayConvention bdc = gc.qlBdc;
    int maxPoints = (options && options->max_points() > 0) ? options->max_points() : 50000;

    if (gridSpec->grid_type() == DateGrid_TenorGrid) {
        auto grid = gridSpec->grid_as_TenorGrid();
        if (!grid || !grid->tenors()) {
            QUANTRA_INVALID_ARGUMENT("TenorGrid.tenors is required");
        }
        dates.reserve(grid->tenors()->size());
        for (flatbuffers::uoffset_t i = 0; i < grid->tenors()->size(); ++i) {
            auto t = grid->tenors()->Get(i);
            QuantLib::Period p(t->n(), TimeUnitToQL(t->unit()));
            if (p.length() == 0 && p.units() != Days) {
                QUANTRA_INVALID_ARGUMENT("TenorGrid only allows zero period as 0 Days");
            }
            Date d = calendar.advance(referenceDate, p, bdc);
            dates.push_back(d);
        }
    } else if (gridSpec->grid_type() == DateGrid_RangeGrid) {
        auto grid = gridSpec->grid_as_RangeGrid();
        if (!grid || !grid->end_date()) {
            QUANTRA_INVALID_ARGUMENT("RangeGrid.end_date is required");
        }
        Date startDate = grid->start_date() ? DateToQL(grid->start_date()->str()) : asOfDate;
        Date endDate = DateToQL(grid->end_date()->str());
        int stepNumber = std::max(1, grid->step_number());
        TimeUnit stepUnit = TimeUnitToQL(grid->step_time_unit());
        QuantLib::Period step(stepNumber, stepUnit);
        bool businessDaysOnly = grid->business_days_only();

        Date current = startDate;
        Date lastAcceptedDate;
        bool hasLastAccepted = false;
        while (current <= endDate) {
            bool accepted = (!businessDaysOnly || calendar.isBusinessDay(current));
            if (accepted) {
                if (hasLastAccepted && current == lastAcceptedDate) {
                    QUANTRA_INVALID_ARGUMENT("RangeGrid produced duplicate points; check step and conventions");
                }
                dates.push_back(current);
                lastAcceptedDate = current;
                hasLastAccepted = true;
            }
            if (static_cast<int>(dates.size()) > maxPoints) {
                QUANTRA_INVALID_ARGUMENT("Grid too large (>" + std::to_string(maxPoints) + " points)");
            }
            if (stepUnit == Days) {
                current = current + stepNumber;
            } else if (stepUnit == Weeks) {
                current = current + 7 * stepNumber;
            } else {
                Date next = calendar.advance(current, step, bdc);
                if (next <= current) {
                    QUANTRA_INVALID_ARGUMENT("RangeGrid step does not advance dates; check step and conventions");
                }
                current = next;
            }
        }
    } else {
        QUANTRA_INVALID_ARGUMENT("DateGridSpec.grid is required");
    }
    return dates;
}

void validateStrictlyIncreasingDates(const std::vector<Date>& dates, const std::string& label) {
    if (dates.empty()) return;
    for (size_t i = 1; i < dates.size(); ++i) {
        if (!(dates[i] > dates[i - 1])) {
            QUANTRA_INVALID_ARGUMENT(label + " must be strictly increasing after conventions");
        }
    }
}

void validateStrictlyIncreasingStrikes(const std::vector<double>& strikes) {
    if (strikes.empty()) return;
    for (size_t i = 1; i < strikes.size(); ++i) {
        if (!(strikes[i] > strikes[i - 1])) {
            QUANTRA_INVALID_ARGUMENT("strike_grid.strikes must be strictly increasing");
        }
    }
}

std::vector<QuantLib::Period> buildTenorPeriods(
    const DateGridSpec* gridSpec) {
    if (!gridSpec || gridSpec->grid_type() != DateGrid_TenorGrid) {
        QUANTRA_INVALID_ARGUMENT("Swaption tenor_grid must be a TenorGrid");
    }
    auto grid = gridSpec->grid_as_TenorGrid();
    if (!grid || !grid->tenors() || grid->tenors()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("Swaption tenor_grid.tenors is required");
    }
    std::vector<QuantLib::Period> periods;
    periods.reserve(grid->tenors()->size());
    for (flatbuffers::uoffset_t i = 0; i < grid->tenors()->size(); ++i) {
        auto t = grid->tenors()->Get(i);
        periods.emplace_back(t->n(), TimeUnitToQL(t->unit()));
    }
    return periods;
}

enums::VolatilityType fromQlVolType(QuantLib::VolatilityType t, double displacement) {
    if (t == QuantLib::Normal) {
        return enums::VolatilityType_Normal;
    }
    if (displacement != 0.0) {
        return enums::VolatilityType_ShiftedLognormal;
    }
    return enums::VolatilityType_Lognormal;
}

std::string toIso(const Date& d) {
    std::ostringstream os;
    os << io::iso_date(d);
    return os.str();
}

double safeOptionTime(const DayCounter& dc, const Date& evalDate, const Date& expiry) {
    return std::max(1.0e-8, dc.yearFraction(evalDate, expiry));
}

int resolveSelectorIndex(int idx, int size, const std::string& name, bool required) {
    if (idx < 0) {
        if (required) {
            QUANTRA_INVALID_ARGUMENT(name + " is required for selected output_mode");
        }
        return 0;
    }
    if (idx >= size) {
        QUANTRA_INVALID_ARGUMENT(name + " out of range: " + std::to_string(idx) +
                     " (size=" + std::to_string(size) + ")");
    }
    return idx;
}

flatbuffers::Offset<quantra::Period> toFbPeriod(flatbuffers::grpc::MessageBuilder& b, const QuantLib::Period& p) {
    quantra::PeriodBuilder pb(b);
    pb.add_n(p.length());
    pb.add_unit(qlTimeUnitToFb(p.units()));
    return pb.Finish();
}

void checkPointBudget(int64_t points, const QueryOptions* options) {
    int maxPoints = (options && options->max_points() > 0) ? options->max_points() : 50000;
    if (points > maxPoints) {
        QUANTRA_INVALID_ARGUMENT("Query exceeds max_points: " + std::to_string(points) +
                      " > " + std::to_string(maxPoints));
    }
}

} // namespace

flatbuffers::Offset<SampleVolSurfacesResponse> SampleVolSurfacesRequestHandler::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const SampleVolSurfacesRequest* request) const {
    auto buildRequestErrorResponse = [&](const std::string& message)
        -> flatbuffers::Offset<SampleVolSurfacesResponse> {
        std::vector<flatbuffers::Offset<VolSurfaceSample>> errorResults;
        if (request && request->queries() && request->queries()->size() > 0) {
            errorResults.reserve(request->queries()->size());
            for (flatbuffers::uoffset_t qi = 0; qi < request->queries()->size(); ++qi) {
                const auto* q = request->queries()->Get(qi);
                std::string volId = (q && q->vol_id()) ? q->vol_id()->str() : "";
                auto volIdOffset = builder->CreateString(volId);
                auto errMsg = builder->CreateString(message);
                ErrorBuilder eb(*builder);
                eb.add_error_message(errMsg);
                auto errOffset = eb.Finish();
                VolSurfaceSampleBuilder out(*builder);
                out.add_vol_id(volIdOffset);
                out.add_error(errOffset);
                errorResults.push_back(out.Finish());
            }
        } else {
            auto volIdOffset = builder->CreateString("");
            auto errMsg = builder->CreateString(message);
            ErrorBuilder eb(*builder);
            eb.add_error_message(errMsg);
            auto errOffset = eb.Finish();
            VolSurfaceSampleBuilder out(*builder);
            out.add_vol_id(volIdOffset);
            out.add_error(errOffset);
            errorResults.push_back(out.Finish());
        }
        auto resultsVec = builder->CreateVector(errorResults);
        SampleVolSurfacesResponseBuilder rb(*builder);
        rb.add_results(resultsVec);
        return rb.Finish();
    };

    try {
        if (!request || !request->pricing() || !request->queries() || request->queries()->size() == 0) {
            QUANTRA_INVALID_ARGUMENT("SampleVolSurfacesRequest.pricing and non-empty queries are required");
        }

        PricingRegistry reg = PricingRegistryBuilder().build(request->pricing());
        const Date asOf = DateToQL(request->pricing()->as_of_date()->str());
        Settings::instance().evaluationDate() = asOf;

        std::vector<flatbuffers::Offset<VolSurfaceSample>> results;
        for (flatbuffers::uoffset_t qi = 0; qi < request->queries()->size(); ++qi) {
            const auto* q = request->queries()->Get(qi);
            std::string volId = (q && q->vol_id()) ? q->vol_id()->str() : "";

        try {
            if (volId.empty()) {
                QUANTRA_INVALID_ARGUMENT("VolQuerySpec.vol_id is required");
            }
            if (!q->strike_grid() || !q->strike_grid()->strikes() || q->strike_grid()->strikes()->size() == 0) {
                QUANTRA_INVALID_ARGUMENT("VolQuerySpec.strike_grid.strikes is required");
            }

            std::vector<flatbuffers::Offset<flatbuffers::String>> expiriesOut;
            std::vector<flatbuffers::Offset<flatbuffers::String>> requestedExpiryGridOut;
            std::vector<flatbuffers::Offset<quantra::Period>> tenorsOut;
            std::vector<flatbuffers::Offset<flatbuffers::String>> effectiveSwapStartsOut;
            std::vector<flatbuffers::Offset<flatbuffers::String>> effectiveSwapEndsOut;
            std::vector<double> strikesOut;
            std::vector<double> volsOut;
            std::vector<double> atmLevelsOut;
            ExpiryKind expiryKindOut = ExpiryKind_GridDate;
            enums::VolatilityType volTypeOut = enums::VolatilityType_Lognormal;
            enums::SwaptionStrikeKind canonicalStrikeKind = enums::SwaptionStrikeKind_Absolute;
            VolStrikeAxis requestedStrikeAxis = q->strike_grid()->axis();
            Date sampleReferenceDate = asOf;
            enums::Calendar usedCalendar = enums::Calendar_NullCalendar;
            enums::BusinessDayConvention usedBdc = enums::BusinessDayConvention_Following;
            bool allowExtrapolationUsed = true;
            int nExpOut = 0, nTenOut = 0, nStrOut = 0;

            if (q->surface_type() == VolSurfaceType_Swaption) {
                auto vIt = reg.volatility.swaptionVols.find(volId);
                if (vIt == reg.volatility.swaptionVols.end()) {
                    QUANTRA_NOT_FOUND("Swaption vol not found: " + volId);
                }
                SwaptionVolEntry volEntry = vIt->second;
                if (volEntry.referenceDate == Date()) {
                    QUANTRA_INVALID_ARGUMENT("Swaption vol has invalid referenceDate: " + volId);
                }
                const auto* options = q->options();
                const bool strictMode = !options || options->strict();
                if (strictMode && volEntry.referenceDate != asOf) {
                    std::ostringstream err;
                    err << "Strict mode: pricing.as_of_date (" << io::iso_date(asOf)
                        << ") must equal swaption vol referenceDate ("
                        << io::iso_date(volEntry.referenceDate) << ") for vol '" << volId << "'";
                    QUANTRA_INVALID_ARGUMENT(err.str());
                }
                if (!q->tenor_grid()) {
                    QUANTRA_INVALID_ARGUMENT("VolQuerySpec.tenor_grid is required for swaption sampling");
                }
                if (!q->expiry_grid()) {
                    QUANTRA_INVALID_ARGUMENT("VolQuerySpec.expiry_grid is required for swaption sampling");
                }
                canonicalStrikeKind = volEntry.strikeKind;
                sampleReferenceDate = volEntry.referenceDate;
                const bool allowExtrapolation = !options || options->allow_extrapolation();
                allowExtrapolationUsed = allowExtrapolation;

                if (volEntry.swapIndexId.empty()) {
                    QUANTRA_INVALID_ARGUMENT("Swaption surface is missing required swap_index_id");
                }
                std::string swapIndexId = volEntry.swapIndexId;
                if (q->swap_index_id() && !q->swap_index_id()->str().empty()) {
                    swapIndexId = q->swap_index_id()->str();
                }
                if (volEntry.swapIndexId != swapIndexId) {
                    QUANTRA_INVALID_ARGUMENT("VolQuerySpec.swap_index_id does not match surface swap_index_id");
                }
                if (!reg.rates.swapIndices.has(swapIndexId)) {
                    QUANTRA_NOT_FOUND("Missing swap index definition for id: " + swapIndexId);
                }
                const SwapIndexRuntime& sidx = reg.rates.swapIndices.get(swapIndexId);
                usedCalendar = sidx.fixedCalendarFb;
                usedBdc = sidx.fixedBdcFb;

                std::vector<Date> rawExpiryGrid;
                std::vector<Date> expiries;
                if (q->expiry_grid()->grid_type() == DateGrid_TenorGrid) {
                    auto expPeriods = buildTenorPeriods(q->expiry_grid());
                    rawExpiryGrid.reserve(expPeriods.size());
                    expiries.reserve(expPeriods.size());
                    for (const auto& p : expPeriods) {
                        Date gridDate = sidx.fixedCalendar.advance(volEntry.referenceDate, p, sidx.fixedBdc);
                        rawExpiryGrid.push_back(gridDate);
                        expiries.push_back(gridDate);
                    }
                } else {
                    rawExpiryGrid = buildDateGrid(
                        q->expiry_grid(), volEntry.referenceDate, asOf, options,
                        sidx.fixedCalendar, sidx.fixedCalendarFb, sidx.fixedBdc, sidx.fixedBdcFb);
                    expiries.reserve(rawExpiryGrid.size());
                    for (const auto& d : rawExpiryGrid) {
                        expiries.push_back(sidx.fixedCalendar.adjust(d, sidx.fixedBdc));
                    }
                }
                for (const auto& d : rawExpiryGrid) {
                    requestedExpiryGridOut.push_back(builder->CreateString(toIso(d)));
                }
                std::vector<QuantLib::Period> tenors = buildTenorPeriods(q->tenor_grid());
                std::vector<double> strikes;
                strikes.reserve(q->strike_grid()->strikes()->size());
                for (flatbuffers::uoffset_t i = 0; i < q->strike_grid()->strikes()->size(); ++i) {
                    strikes.push_back(q->strike_grid()->strikes()->Get(i));
                }
                validateStrictlyIncreasingDates(expiries, "expiry_grid (after swap-index adjustment)");
                validateStrictlyIncreasingStrikes(strikes);

                VolStrikeAxis axis = requestedStrikeAxis;
                if (volEntry.strikeKind == enums::SwaptionStrikeKind_Absolute &&
                    axis == VolStrikeAxis_SpreadFromATM) {
                    QUANTRA_INVALID_ARGUMENT("SpreadFromATM strike axis requested for Absolute-strike swaption vol");
                }

                // SpreadFromATM smile cubes and SABR-params surfaces both need
                // forward resolution before sampling: SpreadFromATM to translate
                // strike spreads to absolute strikes, SABR to instantiate per-node
                // SabrSmileSection from F(expiry, tenor). Both go through the
                // shared finalizeSwaptionVolEntryForPricing path.
                const bool needsForwardResolution =
                    volEntry.strikeKind == enums::SwaptionStrikeKind_SpreadFromATM ||
                    volEntry.volKind == enums::SwaptionVolKind_SabrParams ||
                    volEntry.volKind == enums::SwaptionVolKind_SabrCalibrate;
                if (needsForwardResolution) {
                    if (!q->discounting_curve_id() || q->discounting_curve_id()->str().empty() ||
                        !q->forwarding_curve_id() || q->forwarding_curve_id()->str().empty()) {
                        QUANTRA_INVALID_ARGUMENT(
                            "SpreadFromATM/SABR swaption sampling requires "
                            "discounting_curve_id and forwarding_curve_id");
                    }
                    auto dIt = reg.rates.curves.find(q->discounting_curve_id()->str());
                    auto fIt = reg.rates.curves.find(q->forwarding_curve_id()->str());
                    if (dIt == reg.rates.curves.end() || fIt == reg.rates.curves.end()) {
                        QUANTRA_NOT_FOUND("Sampling curve ids not found for ATM computation");
                    }
                    volEntry = finalizeSwaptionVolEntryForPricing(
                        volEntry,
                        nullptr,
                        reg,
                        Handle<YieldTermStructure>(dIt->second->currentLink()),
                        Handle<YieldTermStructure>(fIt->second->currentLink()),
                        false,
                        q->discounting_curve_id()->str(),
                        q->forwarding_curve_id()->str());
                }

                if (volEntry.handle.empty()) {
                    QUANTRA_ERROR("Swaption vol handle is empty");
                }
                if (allowExtrapolation) volEntry.handle->enableExtrapolation();
                else volEntry.handle->disableExtrapolation();

                int64_t nExp = static_cast<int64_t>(expiries.size());
                int64_t nTen = static_cast<int64_t>(tenors.size());
                int64_t nStr = static_cast<int64_t>(strikes.size());
                VolOutputMode mode = q->output_mode();
                if (mode == VolOutputMode_Cube) checkPointBudget(nExp * nTen * nStr, options);
                else if (mode == VolOutputMode_SmileSlice) checkPointBudget(nStr, options);
                else if (mode == VolOutputMode_TermSlice) checkPointBudget(nTen, options);
                else if (mode == VolOutputMode_ExpirySlice) checkPointBudget(nExp, options);

                const Date evalDate = Settings::instance().evaluationDate();
                int expIdx = 0;
                int tenIdx = 0;

                std::vector<double> precomputedAtm;
                struct SwaptionNodeDates {
                    Date exercise;
                    Date start;
                    Date end;
                };

                auto requireSelectors = [&](VolOutputMode m) {
                    if (m == VolOutputMode_SmileSlice) {
                        expIdx = resolveSelectorIndex(q->slice_expiry_index(), static_cast<int>(expiries.size()), "slice_expiry_index", true);
                        tenIdx = resolveSelectorIndex(q->slice_tenor_index(), static_cast<int>(tenors.size()), "slice_tenor_index", true);
                    } else if (m == VolOutputMode_TermSlice) {
                        expIdx = resolveSelectorIndex(q->slice_expiry_index(), static_cast<int>(expiries.size()), "slice_expiry_index", true);
                        if (!q->slice_strike_is_set()) {
                            QUANTRA_INVALID_ARGUMENT("TermSlice requires slice_strike_is_set=true");
                        }
                        if (!std::isfinite(q->slice_strike())) {
                            QUANTRA_INVALID_ARGUMENT("TermSlice requires finite slice_strike");
                        }
                    } else if (m == VolOutputMode_ExpirySlice) {
                        tenIdx = resolveSelectorIndex(q->slice_tenor_index(), static_cast<int>(tenors.size()), "slice_tenor_index", true);
                        if (!q->slice_strike_is_set()) {
                            QUANTRA_INVALID_ARGUMENT("ExpirySlice requires slice_strike_is_set=true");
                        }
                        if (!std::isfinite(q->slice_strike())) {
                            QUANTRA_INVALID_ARGUMENT("ExpirySlice requires finite slice_strike");
                        }
                    }
                };
                requireSelectors(mode);

                auto atmLookup = [&](int iExp, int iTen) -> double {
                    if (precomputedAtm.empty()) return std::numeric_limits<double>::quiet_NaN();
                    if (mode == VolOutputMode_Cube) {
                        return precomputedAtm[static_cast<size_t>(iExp) * tenors.size() + static_cast<size_t>(iTen)];
                    }
                    if (mode == VolOutputMode_SmileSlice) {
                        return precomputedAtm[0];
                    }
                    if (mode == VolOutputMode_TermSlice) {
                        return precomputedAtm[static_cast<size_t>(iTen)];
                    }
                    // ExpirySlice
                    return precomputedAtm[static_cast<size_t>(iExp)];
                };

                if (volEntry.strikeKind == enums::SwaptionStrikeKind_SpreadFromATM) {
                    if (!q->discounting_curve_id() || q->discounting_curve_id()->str().empty() ||
                        !q->forwarding_curve_id() || q->forwarding_curve_id()->str().empty()) {
                        QUANTRA_INVALID_ARGUMENT("SpreadFromATM requires discounting_curve_id/forwarding_curve_id");
                    }
                    auto dIt = reg.rates.curves.find(q->discounting_curve_id()->str());
                    auto fIt = reg.rates.curves.find(q->forwarding_curve_id()->str());
                    if (dIt == reg.rates.curves.end() || fIt == reg.rates.curves.end()) {
                        QUANTRA_NOT_FOUND("Sampling curve ids not found for ATM computation");
                    }
                    std::vector<Date> atmExpiries;
                    std::vector<QuantLib::Period> atmTenors;
                    if (mode == VolOutputMode_Cube) {
                        atmExpiries = expiries;
                        atmTenors = tenors;
                    } else if (mode == VolOutputMode_SmileSlice) {
                        atmExpiries = {expiries[static_cast<size_t>(expIdx)]};
                        atmTenors = {tenors[static_cast<size_t>(tenIdx)]};
                    } else if (mode == VolOutputMode_TermSlice) {
                        atmExpiries = {expiries[static_cast<size_t>(expIdx)]};
                        atmTenors = tenors;
                    } else { // ExpirySlice
                        atmExpiries = expiries;
                        atmTenors = {tenors[static_cast<size_t>(tenIdx)]};
                    }
                    precomputedAtm = computeServerAtmForwardsForExerciseDates(
                        atmExpiries,
                        atmTenors,
                        sidx,
                        reg.rates.indices,
                        Handle<YieldTermStructure>(dIt->second->currentLink()),
                        Handle<YieldTermStructure>(fIt->second->currentLink()));
                    const size_t expectedAtm = atmExpiries.size() * atmTenors.size();
                    if (precomputedAtm.size() != expectedAtm) {
                        QUANTRA_ERROR(
                            "ATM matrix size mismatch: got " + std::to_string(precomputedAtm.size()) +
                            ", expected " + std::to_string(expectedAtm));
                    }
                }

                auto computeSwaptionDates = [&](int iExp, int iTen) -> SwaptionNodeDates {
                    Date exercise = expiries[static_cast<size_t>(iExp)];
                    QuantLib::Period tenor = tenors[static_cast<size_t>(iTen)];
                    Date start = exercise;
                    if (sidx.spotDays > 0) {
                        start = sidx.fixedCalendar.advance(exercise, sidx.spotDays, Days, sidx.fixedBdc);
                    }
                    Date tentativeEnd = sidx.fixedCalendar.advance(start, tenor, sidx.fixedTermBdc);
                    QuantLib::Schedule fixedSchedule(
                        start, tentativeEnd, QuantLib::Period(sidx.fixedFrequency), sidx.fixedCalendar,
                        sidx.fixedBdc, sidx.fixedTermBdc, sidx.fixedDateRule, sidx.fixedEom);
                    return {exercise, start, fixedSchedule.endDate()};
                };

                // Validate tenor monotonicity using the same date-generation
                // logic used by sampling across representative expiries.
                if (!tenors.empty()) {
                    auto validateTenorMonotonicAtExpiry = [&](int iExp, const std::string& suffix) {
                        std::vector<Date> tenorEnds;
                        tenorEnds.reserve(tenors.size());
                        for (size_t j = 0; j < tenors.size(); ++j) {
                            auto d = computeSwaptionDates(iExp, static_cast<int>(j));
                            tenorEnds.push_back(d.end);
                        }
                        validateStrictlyIncreasingDates(tenorEnds, "tenor_grid" + suffix);
                    };
                    validateTenorMonotonicAtExpiry(0, " (first expiry)");
                    if (expiries.size() > 1) {
                        validateTenorMonotonicAtExpiry(static_cast<int>(expiries.size() - 1), " (last expiry)");
                    }
                }

                auto sampleVol = [&](int iExp, int iTen, double strikeInput, const SwaptionNodeDates& dates) -> double {
                    double optionTime = safeOptionTime(volEntry.dayCounter, evalDate, dates.exercise);
                    double swapLength = std::max(1.0e-8, volEntry.dayCounter.yearFraction(dates.start, dates.end));
                    double atm = std::numeric_limits<double>::quiet_NaN();
                    if (volEntry.strikeKind == enums::SwaptionStrikeKind_SpreadFromATM) {
                        atm = atmLookup(iExp, iTen);
                    }

                    double absStrike = strikeInput;
                    double spread = strikeInput;
                    if (volEntry.strikeKind == enums::SwaptionStrikeKind_SpreadFromATM) {
                        if (axis == VolStrikeAxis_SpreadFromATM) {
                            absStrike = atm + strikeInput;
                            spread = strikeInput;
                        } else {
                            absStrike = strikeInput;
                            spread = strikeInput - atm;
                        }
                        if (!allowExtrapolation && !volEntry.strikes.empty()) {
                            if (spread < volEntry.strikes.front() || spread > volEntry.strikes.back()) {
                                QUANTRA_INVALID_ARGUMENT("Strike/spread is outside smile cube strike support");
                            }
                        }
                    } else if (!allowExtrapolation && !volEntry.strikes.empty()) {
                        if (absStrike < volEntry.strikes.front() || absStrike > volEntry.strikes.back()) {
                            QUANTRA_INVALID_ARGUMENT("Strike is outside swaption vol strike support");
                        }
                    }

                    if (!allowExtrapolation && volEntry.volKind != enums::SwaptionVolKind_Constant) {
                        if (!volEntry.expiries.empty()) {
                            Date minExp = sidx.fixedCalendar.advance(
                                volEntry.referenceDate, volEntry.expiries.front(), sidx.fixedBdc);
                            Date maxExp = sidx.fixedCalendar.advance(
                                volEntry.referenceDate, volEntry.expiries.back(), sidx.fixedBdc);
                            if (dates.exercise < minExp || dates.exercise > maxExp) {
                                QUANTRA_INVALID_ARGUMENT("Expiry is outside swaption vol support");
                            }
                        }
                    }
                    return volEntry.handle->volatility(optionTime, swapLength, absStrike);
                };

                auto computeSwapLengthSupportBounds = [&](const SwaptionNodeDates& dates) -> std::pair<double, double> {
                    if (volEntry.tenors.empty()) {
                        return {0.0, std::numeric_limits<double>::infinity()};
                    }
                    auto computeSwapLengthForSurfaceTenor = [&](const QuantLib::Period& surfaceTenor) {
                        Date boundEnd = sidx.fixedCalendar.advance(dates.start, surfaceTenor, sidx.fixedTermBdc);
                        QuantLib::Schedule fixedSchedule(
                            dates.start, boundEnd, QuantLib::Period(sidx.fixedFrequency), sidx.fixedCalendar,
                            sidx.fixedBdc, sidx.fixedTermBdc, sidx.fixedDateRule, sidx.fixedEom);
                        Date end = fixedSchedule.endDate();
                        return std::max(1.0e-8, volEntry.dayCounter.yearFraction(dates.start, end));
                    };
                    return {
                        computeSwapLengthForSurfaceTenor(volEntry.tenors.front()),
                        computeSwapLengthForSurfaceTenor(volEntry.tenors.back())
                    };
                };

                auto checkTenorSupport = [&](double swapLength, const std::pair<double, double>& bounds) {
                    const double eps = 1.0e-12;
                    if (swapLength < bounds.first - eps || swapLength > bounds.second + eps) {
                        QUANTRA_INVALID_ARGUMENT("Tenor is outside swaption vol support");
                    }
                };

                auto addExpiryLabel = [&](const Date& d) {
                    expiriesOut.push_back(builder->CreateString(toIso(d)));
                };

                if (mode == VolOutputMode_Cube) {
                    for (size_t i = 0; i < expiries.size(); ++i) addExpiryLabel(expiries[i]);
                    for (size_t j = 0; j < tenors.size(); ++j) tenorsOut.push_back(toFbPeriod(*builder, tenors[j]));
                    strikesOut = strikes;
                    expiryKindOut = ExpiryKind_ExerciseDate;

                    for (size_t i = 0; i < expiries.size(); ++i) {
                        for (size_t j = 0; j < tenors.size(); ++j) {
                            auto dates = computeSwaptionDates(static_cast<int>(i), static_cast<int>(j));
                            std::pair<double, double> tenorBounds;
                            bool enforceTenorBounds = (!allowExtrapolation && volEntry.volKind != enums::SwaptionVolKind_Constant);
                            if (enforceTenorBounds) {
                                tenorBounds = computeSwapLengthSupportBounds(dates);
                                double swapLength = std::max(1.0e-8, volEntry.dayCounter.yearFraction(dates.start, dates.end));
                                checkTenorSupport(swapLength, tenorBounds);
                            }
                            effectiveSwapStartsOut.push_back(builder->CreateString(toIso(dates.start)));
                            effectiveSwapEndsOut.push_back(builder->CreateString(toIso(dates.end)));
                            if (volEntry.strikeKind == enums::SwaptionStrikeKind_SpreadFromATM &&
                                !precomputedAtm.empty()) {
                                atmLevelsOut.push_back(atmLookup(static_cast<int>(i), static_cast<int>(j)));
                            }
                            for (double strike : strikes) {
                                volsOut.push_back(sampleVol(static_cast<int>(i), static_cast<int>(j), strike, dates));
                            }
                        }
                    }
                    nExpOut = static_cast<int>(expiries.size());
                    nTenOut = static_cast<int>(tenors.size());
                    nStrOut = static_cast<int>(strikes.size());
                } else if (mode == VolOutputMode_SmileSlice) {
                    int i = expIdx;
                    int j = tenIdx;
                    addExpiryLabel(expiries[static_cast<size_t>(i)]);
                    tenorsOut.push_back(toFbPeriod(*builder, tenors[static_cast<size_t>(j)]));
                    strikesOut = strikes;
                    expiryKindOut = ExpiryKind_ExerciseDate;
                    auto dates = computeSwaptionDates(i, j);
                    std::pair<double, double> tenorBounds;
                    if (!allowExtrapolation && volEntry.volKind != enums::SwaptionVolKind_Constant) {
                        tenorBounds = computeSwapLengthSupportBounds(dates);
                        double swapLength = std::max(1.0e-8, volEntry.dayCounter.yearFraction(dates.start, dates.end));
                        checkTenorSupport(swapLength, tenorBounds);
                    }
                    effectiveSwapStartsOut.push_back(builder->CreateString(toIso(dates.start)));
                    effectiveSwapEndsOut.push_back(builder->CreateString(toIso(dates.end)));
                    if (volEntry.strikeKind == enums::SwaptionStrikeKind_SpreadFromATM) {
                        double atm = atmLookup(i, j);
                        if (std::isfinite(atm)) atmLevelsOut.push_back(atm);
                    }
                    for (double strike : strikes) {
                        volsOut.push_back(sampleVol(i, j, strike, dates));
                    }
                    nExpOut = 1; nTenOut = 1; nStrOut = static_cast<int>(strikes.size());
                } else if (mode == VolOutputMode_TermSlice) {
                    int i = expIdx;
                    addExpiryLabel(expiries[static_cast<size_t>(i)]);
                    strikesOut.push_back(q->slice_strike());
                    expiryKindOut = ExpiryKind_ExerciseDate;
                    for (size_t j = 0; j < tenors.size(); ++j) {
                        auto dates = computeSwaptionDates(i, static_cast<int>(j));
                        std::pair<double, double> tenorBounds;
                        if (!allowExtrapolation && volEntry.volKind != enums::SwaptionVolKind_Constant) {
                            tenorBounds = computeSwapLengthSupportBounds(dates);
                            double swapLength = std::max(1.0e-8, volEntry.dayCounter.yearFraction(dates.start, dates.end));
                            checkTenorSupport(swapLength, tenorBounds);
                        }
                        volsOut.push_back(sampleVol(i, static_cast<int>(j), q->slice_strike(), dates));
                        tenorsOut.push_back(toFbPeriod(*builder, tenors[j]));
                        effectiveSwapStartsOut.push_back(builder->CreateString(toIso(dates.start)));
                        effectiveSwapEndsOut.push_back(builder->CreateString(toIso(dates.end)));
                        if (volEntry.strikeKind == enums::SwaptionStrikeKind_SpreadFromATM) {
                            double atm = atmLookup(i, static_cast<int>(j));
                            if (std::isfinite(atm)) atmLevelsOut.push_back(atm);
                        }
                    }
                    nExpOut = 1; nTenOut = static_cast<int>(tenors.size()); nStrOut = 1;
                } else if (mode == VolOutputMode_ExpirySlice) {
                    int j = tenIdx;
                    strikesOut.push_back(q->slice_strike());
                    tenorsOut.push_back(toFbPeriod(*builder, tenors[static_cast<size_t>(j)]));
                    expiryKindOut = ExpiryKind_ExerciseDate;
                    for (size_t i = 0; i < expiries.size(); ++i) {
                        addExpiryLabel(expiries[i]);
                        auto dates = computeSwaptionDates(static_cast<int>(i), j);
                        std::pair<double, double> tenorBounds;
                        if (!allowExtrapolation && volEntry.volKind != enums::SwaptionVolKind_Constant) {
                            tenorBounds = computeSwapLengthSupportBounds(dates);
                            double swapLength = std::max(1.0e-8, volEntry.dayCounter.yearFraction(dates.start, dates.end));
                            checkTenorSupport(swapLength, tenorBounds);
                        }
                        volsOut.push_back(sampleVol(static_cast<int>(i), j, q->slice_strike(), dates));
                        effectiveSwapStartsOut.push_back(builder->CreateString(toIso(dates.start)));
                        effectiveSwapEndsOut.push_back(builder->CreateString(toIso(dates.end)));
                        if (!precomputedAtm.empty()) {
                            double atm = atmLookup(static_cast<int>(i), j);
                            if (std::isfinite(atm)) atmLevelsOut.push_back(atm);
                        }
                    }
                    nExpOut = static_cast<int>(expiries.size()); nTenOut = 1; nStrOut = 1;
                } else {
                    QUANTRA_INVALID_ARGUMENT("Unsupported VolOutputMode");
                }

                volTypeOut = fromQlVolType(volEntry.qlVolType, volEntry.displacement);
            } else if (q->surface_type() == VolSurfaceType_EquityBlack) {
                auto vIt = reg.volatility.blackVols.find(volId);
                if (vIt == reg.volatility.blackVols.end()) {
                    QUANTRA_NOT_FOUND("Black vol not found: " + volId);
                }
                const BlackVolEntry& volEntry = vIt->second;

                if (!q->expiry_grid()) {
                    QUANTRA_INVALID_ARGUMENT("VolQuerySpec.expiry_grid is required for equity black sampling");
                }
                if (q->output_mode() != VolOutputMode_Cube) {
                    QUANTRA_INVALID_ARGUMENT("EquityBlack sampling supports Cube output_mode only");
                }
                if (q->strike_grid()->axis() == VolStrikeAxis_SpreadFromATM) {
                    QUANTRA_INVALID_ARGUMENT("EquityBlack sampling supports AbsoluteStrike axis only");
                }
                if (q->tenor_grid()) {
                    QUANTRA_INVALID_ARGUMENT("tenor_grid is not valid for equity black sampling");
                }
                if (q->swap_index_id() && !q->swap_index_id()->str().empty()) {
                    QUANTRA_INVALID_ARGUMENT("swap_index_id is not valid for equity black sampling");
                }
                if ((q->discounting_curve_id() && !q->discounting_curve_id()->str().empty()) ||
                    (q->forwarding_curve_id() && !q->forwarding_curve_id()->str().empty())) {
                    QUANTRA_INVALID_ARGUMENT(
                        "discounting_curve_id/forwarding_curve_id are not valid for equity black sampling");
                }
                if (q->slice_expiry_index() >= 0 || q->slice_tenor_index() >= 0 || q->slice_strike_is_set()) {
                    QUANTRA_INVALID_ARGUMENT("EquityBlack query does not support slice selectors");
                }
                const auto* options = q->options();
                const bool strictMode = !options || options->strict();
                if (strictMode && volEntry.referenceDate != asOf) {
                    std::ostringstream err;
                    err << "Strict mode: pricing.as_of_date (" << io::iso_date(asOf)
                        << ") must equal equity black vol referenceDate ("
                        << io::iso_date(volEntry.referenceDate) << ") for vol '" << volId << "'";
                    QUANTRA_INVALID_ARGUMENT(err.str());
                }
                sampleReferenceDate = volEntry.referenceDate;
                const bool allowExtrapolation = !options || options->allow_extrapolation();
                allowExtrapolationUsed = allowExtrapolation;
                canonicalStrikeKind = enums::SwaptionStrikeKind_Absolute;
                expiryKindOut = ExpiryKind_GridDate;
                volTypeOut = enums::VolatilityType_Lognormal;

                GridConventions gc = resolveGridConventions(
                    q->expiry_grid(), options, volEntry.calendar, volEntry.calendarFb,
                    volEntry.businessDayConvention, volEntry.businessDayConventionFb);
                usedCalendar = gc.fbCalendar;
                usedBdc = gc.fbBdc;

                if (allowExtrapolation) volEntry.handle->enableExtrapolation();
                else volEntry.handle->disableExtrapolation();

                std::vector<Date> expiries = buildDateGrid(
                    q->expiry_grid(), sampleReferenceDate, asOf, options,
                    volEntry.calendar, gc.fbCalendar,
                    volEntry.businessDayConvention, volEntry.businessDayConventionFb);
                validateStrictlyIncreasingDates(expiries, "expiry_grid");
                for (const auto& d : expiries) {
                    expiriesOut.push_back(builder->CreateString(toIso(d)));
                }

                for (flatbuffers::uoffset_t i = 0; i < q->strike_grid()->strikes()->size(); ++i) {
                    strikesOut.push_back(q->strike_grid()->strikes()->Get(i));
                }
                validateStrictlyIncreasingStrikes(strikesOut);

                checkPointBudget(
                    static_cast<int64_t>(expiries.size()) * static_cast<int64_t>(strikesOut.size()),
                    options);
                for (const auto& d : expiries) {
                    for (double strike : strikesOut) {
                        if (!allowExtrapolation && d > volEntry.handle->maxDate()) {
                            QUANTRA_INVALID_ARGUMENT("Expiry is outside equity black vol support");
                        }
                        if (!allowExtrapolation) {
                            // Strike support bounds are term-structure dependent in QuantLib implementations.
                            if (strike < volEntry.handle->minStrike() || strike > volEntry.handle->maxStrike()) {
                                QUANTRA_INVALID_ARGUMENT("Strike is outside equity black vol support");
                            }
                        }
                        volsOut.push_back(volEntry.handle->blackVol(d, strike));
                    }
                }
                nExpOut = static_cast<int>(expiries.size());
                nTenOut = 0;
                nStrOut = static_cast<int>(strikesOut.size());
            } else if (q->surface_type() == VolSurfaceType_Optionlet) {
                auto vIt = reg.volatility.optionletVols.find(volId);
                if (vIt == reg.volatility.optionletVols.end()) {
                    QUANTRA_NOT_FOUND("Optionlet vol not found: " + volId);
                }
                const OptionletVolEntry& volEntry = vIt->second;
                const auto* options = q->options();
                const bool strictMode = !options || options->strict();
                if (strictMode && volEntry.referenceDate != asOf) {
                    std::ostringstream err;
                    err << "Strict mode: pricing.as_of_date (" << io::iso_date(asOf)
                        << ") must equal optionlet vol referenceDate ("
                        << io::iso_date(volEntry.referenceDate) << ") for vol '" << volId << "'";
                    QUANTRA_INVALID_ARGUMENT(err.str());
                }
                if (q->strike_grid()->axis() == VolStrikeAxis_SpreadFromATM) {
                    QUANTRA_INVALID_ARGUMENT("Optionlet sampling supports AbsoluteStrike axis only");
                }
                if (q->output_mode() != VolOutputMode_Cube) {
                    QUANTRA_INVALID_ARGUMENT("Optionlet sampling supports Cube output_mode only");
                }
                if (q->swap_index_id() && !q->swap_index_id()->str().empty()) {
                    QUANTRA_INVALID_ARGUMENT("swap_index_id is not valid for optionlet sampling");
                }
                if (q->slice_expiry_index() >= 0 || q->slice_tenor_index() >= 0 || q->slice_strike_is_set()) {
                    QUANTRA_INVALID_ARGUMENT("Optionlet query does not support slice selectors");
                }
                const bool allowExtrapolation = !options || options->allow_extrapolation();
                sampleReferenceDate = volEntry.referenceDate;
                canonicalStrikeKind = enums::SwaptionStrikeKind_Absolute;
                GridConventions gc = resolveGridConventions(
                    q->expiry_grid(), options, volEntry.calendar, volEntry.calendarFb,
                    volEntry.businessDayConvention, volEntry.businessDayConventionFb);
                usedCalendar = gc.fbCalendar;
                usedBdc = gc.fbBdc;
                allowExtrapolationUsed = allowExtrapolation;
                expiryKindOut = ExpiryKind_GridDate;
                if (allowExtrapolation) volEntry.handle->enableExtrapolation();
                else volEntry.handle->disableExtrapolation();
                std::vector<Date> expiries = buildDateGrid(
                    q->expiry_grid(), volEntry.referenceDate, asOf, options,
                    volEntry.calendar, gc.fbCalendar,
                    volEntry.businessDayConvention, volEntry.businessDayConventionFb);
                validateStrictlyIncreasingDates(expiries, "expiry_grid");
                for (size_t i = 0; i < expiries.size(); ++i) {
                    expiriesOut.push_back(builder->CreateString(toIso(expiries[i])));
                }
                for (flatbuffers::uoffset_t i = 0; i < q->strike_grid()->strikes()->size(); ++i) {
                    strikesOut.push_back(q->strike_grid()->strikes()->Get(i));
                }
                validateStrictlyIncreasingStrikes(strikesOut);
                checkPointBudget(
                    static_cast<int64_t>(expiries.size()) * static_cast<int64_t>(strikesOut.size()),
                    options);
                for (const auto& d : expiries) {
                    for (double strike : strikesOut) {
                        if (!allowExtrapolation && d > volEntry.handle->maxDate()) {
                            QUANTRA_INVALID_ARGUMENT("Expiry is outside optionlet vol support");
                        }
                        if (!allowExtrapolation) {
                            if (strike < volEntry.handle->minStrike() || strike > volEntry.handle->maxStrike()) {
                                QUANTRA_INVALID_ARGUMENT("Strike is outside optionlet vol support");
                            }
                        }
                        volsOut.push_back(volEntry.handle->volatility(d, strike));
                    }
                }
                nExpOut = static_cast<int>(expiries.size());
                nTenOut = 0;
                nStrOut = static_cast<int>(strikesOut.size());
                volTypeOut = fromQlVolType(volEntry.qlVolType, volEntry.displacement);
            } else {
                QUANTRA_INVALID_ARGUMENT("Unknown VolSurfaceType");
            }

            auto volIdOffset = builder->CreateString(volId);
            auto refDateOffset = builder->CreateString(toIso(sampleReferenceDate));
            auto expiriesVec = builder->CreateVector(expiriesOut);
            auto requestedExpiriesVec = builder->CreateVector(requestedExpiryGridOut);
            auto tenorsVec = builder->CreateVector(tenorsOut);
            auto effectiveStartsVec = builder->CreateVector(effectiveSwapStartsOut);
            auto effectiveEndsVec = builder->CreateVector(effectiveSwapEndsOut);
            auto strikesVec = builder->CreateVector(strikesOut);
            auto volsVec = builder->CreateVector(volsOut);
            auto atmVec = builder->CreateVector(atmLevelsOut);

            VolSurfaceSampleBuilder out(*builder);
            out.add_vol_id(volIdOffset);
            out.add_reference_date(refDateOffset);
            out.add_ql_vol_type(volTypeOut);
            out.add_requested_strike_axis(requestedStrikeAxis);
            out.add_canonical_strike_kind(canonicalStrikeKind);
            out.add_allow_extrapolation_used(allowExtrapolationUsed);
            out.add_calendar_used(usedCalendar);
            out.add_business_day_convention_used(usedBdc);
            out.add_expiry_kind(expiryKindOut);
            out.add_expiries(expiriesVec);
            out.add_requested_expiry_grid_points(requestedExpiriesVec);
            out.add_tenors(tenorsVec);
            out.add_effective_swap_starts(effectiveStartsVec);
            out.add_effective_swap_ends(effectiveEndsVec);
            out.add_strikes(strikesVec);
            out.add_vols(volsVec);
            out.add_n_expiries(nExpOut);
            out.add_n_tenors(nTenOut);
            out.add_n_strikes(nStrOut);
            out.add_atm_levels(atmVec);
            results.push_back(out.Finish());
        } catch (const std::exception& e) {
            auto volIdOffset = builder->CreateString(volId);
            auto errMsg = builder->CreateString(e.what());
            ErrorBuilder eb(*builder);
            eb.add_error_message(errMsg);
            auto errOffset = eb.Finish();

            VolSurfaceSampleBuilder out(*builder);
            out.add_vol_id(volIdOffset);
            out.add_error(errOffset);
            results.push_back(out.Finish());
        }
        }

        if (results.empty()) {
            QUANTRA_ERROR("SampleVolSurfacesRequest produced no results");
        }

        std::vector<flatbuffers::Offset<quantra::SwaptionVolDiagnostics>> diagnosticsOffs;
        if (request->include_diagnostics()) {
            // Collect unique SABR-kind vol_ids (in first-seen order) along with
            // the curve ids supplied by the query. Diagnostics are emitted only
            // for SABR surfaces — other vol kinds don't fit the schema's SABR-
            // shaped diagnostics block.
            struct DiagJob {
                std::string volId;
                std::string discountCurveId;
                std::string forwardingCurveId;
            };
            std::vector<DiagJob> jobs;
            std::unordered_map<std::string, int> seen;
            for (flatbuffers::uoffset_t qi = 0; qi < request->queries()->size(); ++qi) {
                const auto* q = request->queries()->Get(qi);
                if (!q || !q->vol_id()) continue;
                std::string vid = q->vol_id()->str();
                if (vid.empty()) continue;
                auto it = reg.volatility.swaptionVols.find(vid);
                if (it == reg.volatility.swaptionVols.end()) continue;
                const auto& e = it->second;
                if (e.volKind != enums::SwaptionVolKind_SabrCalibrate &&
                    e.volKind != enums::SwaptionVolKind_SabrParams) {
                    continue;
                }
                if (seen.count(vid)) continue;
                seen[vid] = static_cast<int>(jobs.size());
                DiagJob j;
                j.volId = vid;
                if (q->discounting_curve_id()) j.discountCurveId = q->discounting_curve_id()->str();
                if (q->forwarding_curve_id()) j.forwardingCurveId = q->forwarding_curve_id()->str();
                jobs.push_back(j);
            }

            for (const auto& job : jobs) {
                auto it = reg.volatility.swaptionVols.find(job.volId);
                if (it == reg.volatility.swaptionVols.end()) continue;
                SwaptionVolEntry entry = it->second;
                std::vector<std::string> warnings;
                bool ok = true;
                try {
                    auto dIt = reg.rates.curves.find(job.discountCurveId);
                    auto fIt = reg.rates.curves.find(job.forwardingCurveId);
                    if (dIt == reg.rates.curves.end() || fIt == reg.rates.curves.end()) {
                        // Sampler couldn't resolve curves for this query; emit
                        // partial diagnostics rather than abort the response.
                        warnings.push_back(
                            "diagnostics: curve ids for vol_id '" + job.volId +
                            "' not resolvable from any query; skipping full finalize");
                        ok = false;
                    } else {
                        entry = finalizeSwaptionVolEntryForPricing(
                            entry, nullptr, reg,
                            Handle<YieldTermStructure>(dIt->second->currentLink()),
                            Handle<YieldTermStructure>(fIt->second->currentLink()),
                            false,
                            job.discountCurveId,
                            job.forwardingCurveId);
                    }
                } catch (const std::exception& e) {
                    warnings.push_back(
                        std::string("diagnostics: finalize failed for vol_id '") +
                        job.volId + "': " + e.what());
                    ok = false;
                }
                if (ok) {
                    diagnosticsOffs.push_back(
                        buildSwaptionVolDiagnostics(*builder, job.volId, entry, warnings));
                } else {
                    diagnosticsOffs.push_back(
                        buildPartialSwaptionVolDiagnostics(
                            *builder, job.volId, it->second.volKind, warnings));
                }
            }
        }

        auto resultsVec = builder->CreateVector(results);
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwaptionVolDiagnostics>>> diagVec = 0;
        if (!diagnosticsOffs.empty()) {
            diagVec = builder->CreateVector(diagnosticsOffs);
        }
        SampleVolSurfacesResponseBuilder rb(*builder);
        rb.add_results(resultsVec);
        if (diagVec.o != 0) rb.add_diagnostics(diagVec);
        return rb.Finish();
    } catch (const std::exception& e) {
        return buildRequestErrorResponse(e.what());
    }
}
