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
    enums::Calendar fallbackFbCalendar) {
    GridConventions gc;
    gc.qlCalendar = fallbackCalendar;
    gc.fbCalendar = fallbackFbCalendar;
    gc.qlBdc = Following;
    gc.fbBdc = enums::BusinessDayConvention_Following;

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
    if (options) {
        gc.qlBdc = ConventionToQL(options->business_day_convention());
        gc.fbBdc = options->business_day_convention();
        return gc;
    }
    if (!gridSpec) return gc;
    if (gridSpec->grid_type() == DateGrid_TenorGrid) {
        gc.qlBdc = ConventionToQL(gridSpec->grid_as_TenorGrid()->business_day_convention());
        gc.fbBdc = gridSpec->grid_as_TenorGrid()->business_day_convention();
        return gc;
    }
    if (gridSpec->grid_type() == DateGrid_RangeGrid) {
        gc.qlBdc = ConventionToQL(gridSpec->grid_as_RangeGrid()->business_day_convention());
        gc.fbBdc = gridSpec->grid_as_RangeGrid()->business_day_convention();
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
    enums::Calendar fallbackFbCalendar) {
    if (!gridSpec) {
        QUANTRA_ERROR("DateGridSpec is required");
    }
    std::vector<Date> dates;
    GridConventions gc = resolveGridConventions(
        gridSpec, options, fallbackCalendar, fallbackFbCalendar);
    Calendar calendar = gc.qlCalendar;
    BusinessDayConvention bdc = gc.qlBdc;
    int maxPoints = (options && options->max_points() > 0) ? options->max_points() : 50000;

    if (gridSpec->grid_type() == DateGrid_TenorGrid) {
        auto grid = gridSpec->grid_as_TenorGrid();
        if (!grid || !grid->tenors()) {
            QUANTRA_ERROR("TenorGrid.tenors is required");
        }
        dates.reserve(grid->tenors()->size());
        for (flatbuffers::uoffset_t i = 0; i < grid->tenors()->size(); ++i) {
            auto t = grid->tenors()->Get(i);
            QuantLib::Period p(t->n(), TimeUnitToQL(t->unit()));
            if (p.length() == 0 && p.units() != Days) {
                QUANTRA_ERROR("TenorGrid only allows zero period as 0 Days");
            }
            Date d = calendar.advance(referenceDate, p, bdc);
            dates.push_back(d);
        }
    } else if (gridSpec->grid_type() == DateGrid_RangeGrid) {
        auto grid = gridSpec->grid_as_RangeGrid();
        if (!grid || !grid->end_date()) {
            QUANTRA_ERROR("RangeGrid.end_date is required");
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
                    QUANTRA_ERROR("RangeGrid produced duplicate points; check step and conventions");
                }
                dates.push_back(current);
                lastAcceptedDate = current;
                hasLastAccepted = true;
            }
            if (static_cast<int>(dates.size()) > maxPoints) {
                QUANTRA_ERROR("Grid too large (>" + std::to_string(maxPoints) + " points)");
            }
            if (stepUnit == Days) {
                current = current + stepNumber;
            } else if (stepUnit == Weeks) {
                current = current + 7 * stepNumber;
            } else {
                Date next = calendar.advance(current, step, bdc);
                if (next <= current) {
                    QUANTRA_ERROR("RangeGrid step does not advance dates; check step and conventions");
                }
                current = next;
            }
        }
    } else {
        QUANTRA_ERROR("DateGridSpec.grid is required");
    }
    return dates;
}

void validateStrictlyIncreasingDates(const std::vector<Date>& dates, const std::string& label) {
    if (dates.empty()) return;
    for (size_t i = 1; i < dates.size(); ++i) {
        if (!(dates[i] > dates[i - 1])) {
            QUANTRA_ERROR(label + " must be strictly increasing after conventions");
        }
    }
}

void validateStrictlyIncreasingStrikes(const std::vector<double>& strikes) {
    if (strikes.empty()) return;
    for (size_t i = 1; i < strikes.size(); ++i) {
        if (!(strikes[i] > strikes[i - 1])) {
            QUANTRA_ERROR("strike_grid.strikes must be strictly increasing");
        }
    }
}

std::vector<QuantLib::Period> buildTenorPeriods(
    const DateGridSpec* gridSpec) {
    if (!gridSpec || gridSpec->grid_type() != DateGrid_TenorGrid) {
        QUANTRA_ERROR("Swaption tenor_grid must be a TenorGrid");
    }
    auto grid = gridSpec->grid_as_TenorGrid();
    if (!grid || !grid->tenors() || grid->tenors()->size() == 0) {
        QUANTRA_ERROR("Swaption tenor_grid.tenors is required");
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
            QUANTRA_ERROR(name + " is required for selected output_mode");
        }
        return 0;
    }
    if (idx >= size) {
        QUANTRA_ERROR(name + " out of range: " + std::to_string(idx) +
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
        QUANTRA_ERROR("Query exceeds max_points: " + std::to_string(points) +
                      " > " + std::to_string(maxPoints));
    }
}

} // namespace

flatbuffers::Offset<SampleVolSurfacesResponse> SampleVolSurfacesRequestHandler::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const SampleVolSurfacesRequest* request) const {
    if (!request || !request->pricing() || !request->queries()) {
        QUANTRA_ERROR("SampleVolSurfacesRequest.pricing and queries are required");
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
                QUANTRA_ERROR("VolQuerySpec.vol_id is required");
            }
            if (!q->strike_grid() || !q->strike_grid()->strikes() || q->strike_grid()->strikes()->size() == 0) {
                QUANTRA_ERROR("VolQuerySpec.strike_grid.strikes is required");
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
                auto vIt = reg.swaptionVols.find(volId);
                if (vIt == reg.swaptionVols.end()) {
                    QUANTRA_ERROR("Swaption vol not found: " + volId);
                }
                SwaptionVolEntry volEntry = vIt->second;
                if (volEntry.referenceDate == Date()) {
                    QUANTRA_ERROR("Swaption vol has invalid referenceDate: " + volId);
                }
                if (volEntry.referenceDate != asOf) {
                    std::ostringstream err;
                    err << "Strict mode: pricing.as_of_date (" << io::iso_date(asOf)
                        << ") must equal swaption vol referenceDate ("
                        << io::iso_date(volEntry.referenceDate) << ") for vol '" << volId << "'";
                    QUANTRA_ERROR(err.str());
                }
                if (!q->tenor_grid()) {
                    QUANTRA_ERROR("VolQuerySpec.tenor_grid is required for swaption sampling");
                }
                if (!q->expiry_grid()) {
                    QUANTRA_ERROR("VolQuerySpec.expiry_grid is required for swaption sampling");
                }
                canonicalStrikeKind = volEntry.strikeKind;
                sampleReferenceDate = volEntry.referenceDate;

                const auto* options = q->options();
                const bool allowExtrapolation = !options || options->allow_extrapolation();
                allowExtrapolationUsed = allowExtrapolation;

                if (volEntry.swapIndexId.empty()) {
                    QUANTRA_ERROR("Swaption surface is missing required swap_index_id");
                }
                std::string swapIndexId = volEntry.swapIndexId;
                if (q->swap_index_id() && !q->swap_index_id()->str().empty()) {
                    swapIndexId = q->swap_index_id()->str();
                }
                if (volEntry.swapIndexId != swapIndexId) {
                    QUANTRA_ERROR("VolQuerySpec.swap_index_id does not match surface swap_index_id");
                }
                if (!reg.swapIndices.has(swapIndexId)) {
                    QUANTRA_ERROR("Missing swap index definition for id: " + swapIndexId);
                }
                const SwapIndexRuntime& sidx = reg.swapIndices.get(swapIndexId);
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
                        sidx.fixedCalendar, sidx.fixedCalendarFb);
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
                    QUANTRA_ERROR("SpreadFromATM strike axis requested for Absolute-strike swaption vol");
                }

                if (volEntry.strikeKind == enums::SwaptionStrikeKind_SpreadFromATM) {
                    if (!q->discounting_curve_id() || q->discounting_curve_id()->str().empty() ||
                        !q->forwarding_curve_id() || q->forwarding_curve_id()->str().empty()) {
                        QUANTRA_ERROR(
                            "SpreadFromATM swaption sampling requires discounting_curve_id and forwarding_curve_id");
                    }
                    auto dIt = reg.curves.find(q->discounting_curve_id()->str());
                    auto fIt = reg.curves.find(q->forwarding_curve_id()->str());
                    if (dIt == reg.curves.end() || fIt == reg.curves.end()) {
                        QUANTRA_ERROR("Sampling curve ids not found for ATM computation");
                    }
                    volEntry = finalizeSwaptionVolEntryForPricing(
                        volEntry,
                        nullptr,
                        reg,
                        Handle<YieldTermStructure>(dIt->second->currentLink()),
                        Handle<YieldTermStructure>(fIt->second->currentLink()),
                        false);
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
                            QUANTRA_ERROR("TermSlice requires slice_strike_is_set=true");
                        }
                        if (!std::isfinite(q->slice_strike())) {
                            QUANTRA_ERROR("TermSlice requires finite slice_strike");
                        }
                    } else if (m == VolOutputMode_ExpirySlice) {
                        tenIdx = resolveSelectorIndex(q->slice_tenor_index(), static_cast<int>(tenors.size()), "slice_tenor_index", true);
                        if (!q->slice_strike_is_set()) {
                            QUANTRA_ERROR("ExpirySlice requires slice_strike_is_set=true");
                        }
                        if (!std::isfinite(q->slice_strike())) {
                            QUANTRA_ERROR("ExpirySlice requires finite slice_strike");
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
                        QUANTRA_ERROR("SpreadFromATM requires discounting_curve_id/forwarding_curve_id");
                    }
                    auto dIt = reg.curves.find(q->discounting_curve_id()->str());
                    auto fIt = reg.curves.find(q->forwarding_curve_id()->str());
                    if (dIt == reg.curves.end() || fIt == reg.curves.end()) {
                        QUANTRA_ERROR("Sampling curve ids not found for ATM computation");
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
                        reg.indices,
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
                                QUANTRA_ERROR("Strike/spread is outside smile cube strike support");
                            }
                        }
                    } else if (!allowExtrapolation && !volEntry.strikes.empty()) {
                        if (absStrike < volEntry.strikes.front() || absStrike > volEntry.strikes.back()) {
                            QUANTRA_ERROR("Strike is outside swaption vol strike support");
                        }
                    }

                    if (!allowExtrapolation && volEntry.volKind != enums::SwaptionVolKind_Constant) {
                        if (!volEntry.expiries.empty()) {
                            Date minExp = sidx.fixedCalendar.advance(
                                volEntry.referenceDate, volEntry.expiries.front(), sidx.fixedBdc);
                            Date maxExp = sidx.fixedCalendar.advance(
                                volEntry.referenceDate, volEntry.expiries.back(), sidx.fixedBdc);
                            if (dates.exercise < minExp || dates.exercise > maxExp) {
                                QUANTRA_ERROR("Expiry is outside swaption vol support");
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
                        QUANTRA_ERROR("Tenor is outside swaption vol support");
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
                        if (volEntry.strikeKind == enums::SwaptionStrikeKind_SpreadFromATM) {
                            double atm = atmLookup(static_cast<int>(i), j);
                            if (std::isfinite(atm)) atmLevelsOut.push_back(atm);
                        }
                    }
                    nExpOut = static_cast<int>(expiries.size()); nTenOut = 1; nStrOut = 1;
                } else {
                    QUANTRA_ERROR("Unsupported VolOutputMode");
                }

                volTypeOut = fromQlVolType(volEntry.qlVolType, volEntry.displacement);
            } else if (q->surface_type() == VolSurfaceType_Optionlet) {
                auto vIt = reg.optionletVols.find(volId);
                if (vIt == reg.optionletVols.end()) {
                    QUANTRA_ERROR("Optionlet vol not found: " + volId);
                }
                const OptionletVolEntry& volEntry = vIt->second;
                if (volEntry.referenceDate != asOf) {
                    std::ostringstream err;
                    err << "Strict mode: pricing.as_of_date (" << io::iso_date(asOf)
                        << ") must equal optionlet vol referenceDate ("
                        << io::iso_date(volEntry.referenceDate) << ") for vol '" << volId << "'";
                    QUANTRA_ERROR(err.str());
                }
                if (q->strike_grid()->axis() == VolStrikeAxis_SpreadFromATM) {
                    QUANTRA_ERROR("Optionlet sampling supports AbsoluteStrike axis only");
                }
                if (q->output_mode() != VolOutputMode_Cube) {
                    QUANTRA_ERROR("Optionlet sampling supports Cube output_mode only");
                }
                if (q->swap_index_id() && !q->swap_index_id()->str().empty()) {
                    QUANTRA_ERROR("swap_index_id is not valid for optionlet sampling");
                }
                if (q->slice_expiry_index() >= 0 || q->slice_tenor_index() >= 0 || q->slice_strike_is_set()) {
                    QUANTRA_ERROR("Optionlet query does not support slice selectors");
                }
                const auto* options = q->options();
                const bool allowExtrapolation = !options || options->allow_extrapolation();
                sampleReferenceDate = volEntry.referenceDate;
                canonicalStrikeKind = enums::SwaptionStrikeKind_Absolute;
                GridConventions gc = resolveGridConventions(
                    q->expiry_grid(), options, volEntry.calendar, volEntry.calendarFb);
                usedCalendar = gc.fbCalendar;
                usedBdc = gc.fbBdc;
                allowExtrapolationUsed = allowExtrapolation;
                expiryKindOut = ExpiryKind_GridDate;
                if (allowExtrapolation) volEntry.handle->enableExtrapolation();
                else volEntry.handle->disableExtrapolation();
                std::vector<Date> expiries = buildDateGrid(
                    q->expiry_grid(), volEntry.referenceDate, asOf, options,
                    volEntry.calendar, gc.fbCalendar);
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
                            QUANTRA_ERROR("Expiry is outside optionlet vol support");
                        }
                        if (!allowExtrapolation) {
                            if (strike < volEntry.handle->minStrike() || strike > volEntry.handle->maxStrike()) {
                                QUANTRA_ERROR("Strike is outside optionlet vol support");
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
                QUANTRA_ERROR("Unknown VolSurfaceType");
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

    auto resultsVec = builder->CreateVector(results);
    SampleVolSurfacesResponseBuilder rb(*builder);
    rb.add_results(resultsVec);
    return rb.Finish();
}
