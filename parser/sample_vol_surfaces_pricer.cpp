#include "sample_vol_surfaces_pricer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <ql/quantlib.hpp>

#include "common.h"
#include "enums.h"
#include "error.h"
#include "swaption_vol_runtime.h"

using namespace QuantLib;

namespace quantra {

namespace {

struct GridConventions {
    Calendar qlCalendar;
    quantra::enums::Calendar fbCalendar;
    BusinessDayConvention qlBdc;
    quantra::enums::BusinessDayConvention fbBdc;
};

GridConventions resolveGridConventions(
    const SampleDateGridSpec& gridSpec,
    const SampleQueryOptions& options,
    const Calendar& fallbackCalendar,
    quantra::enums::Calendar fallbackFbCalendar,
    BusinessDayConvention fallbackBdc,
    quantra::enums::BusinessDayConvention fallbackFbBdc) {
    GridConventions gc;
    gc.qlCalendar = fallbackCalendar;
    gc.fbCalendar = fallbackFbCalendar;
    gc.qlBdc = fallbackBdc;
    gc.fbBdc = fallbackFbBdc;
    auto shouldUseOverrideBdc = [](quantra::enums::BusinessDayConvention bdc, quantra::enums::Calendar cal) {
        // QueryOptions/TenorGrid/RangeGrid default to Following even when omitted.
        // Treat Following as "no explicit override" unless calendar is explicitly set.
        return cal != quantra::enums::Calendar_NullCalendar || bdc != quantra::enums::BusinessDayConvention_Following;
    };

    if (options.present && options.calendar != quantra::enums::Calendar_NullCalendar) {
        gc.qlCalendar = CalendarToQL(options.calendar);
        gc.fbCalendar = options.calendar;
    } else if (gridSpec.present && gridSpec.kind == SampleDateGridKind::Tenor) {
        if (gridSpec.tenor.calendar != quantra::enums::Calendar_NullCalendar) {
            gc.qlCalendar = CalendarToQL(gridSpec.tenor.calendar);
            gc.fbCalendar = gridSpec.tenor.calendar;
        }
    } else if (gridSpec.present && gridSpec.kind == SampleDateGridKind::Range) {
        if (gridSpec.range.calendar != quantra::enums::Calendar_NullCalendar) {
            gc.qlCalendar = CalendarToQL(gridSpec.range.calendar);
            gc.fbCalendar = gridSpec.range.calendar;
        }
    }
    if (options.present && shouldUseOverrideBdc(options.businessDayConvention, options.calendar)) {
        gc.qlBdc = ConventionToQL(options.businessDayConvention);
        gc.fbBdc = options.businessDayConvention;
        return gc;
    }
    if (!gridSpec.present) return gc;
    if (gridSpec.kind == SampleDateGridKind::Tenor) {
        if (shouldUseOverrideBdc(gridSpec.tenor.businessDayConvention, gridSpec.tenor.calendar)) {
            gc.qlBdc = ConventionToQL(gridSpec.tenor.businessDayConvention);
            gc.fbBdc = gridSpec.tenor.businessDayConvention;
        }
        return gc;
    }
    if (gridSpec.kind == SampleDateGridKind::Range) {
        if (shouldUseOverrideBdc(gridSpec.range.businessDayConvention, gridSpec.range.calendar)) {
            gc.qlBdc = ConventionToQL(gridSpec.range.businessDayConvention);
            gc.fbBdc = gridSpec.range.businessDayConvention;
        }
        return gc;
    }
    return gc;
}

std::vector<Date> buildDateGrid(
    const SampleDateGridSpec& gridSpec,
    const Date& referenceDate,
    const Date& asOfDate,
    const SampleQueryOptions& options,
    const Calendar& fallbackCalendar,
    quantra::enums::Calendar fallbackFbCalendar,
    BusinessDayConvention fallbackBdc,
    quantra::enums::BusinessDayConvention fallbackFbBdc) {
    if (!gridSpec.present) {
        QUANTRA_INVALID_ARGUMENT("DateGridSpec is required");
    }
    std::vector<Date> dates;
    GridConventions gc = resolveGridConventions(
        gridSpec, options, fallbackCalendar, fallbackFbCalendar, fallbackBdc, fallbackFbBdc);
    Calendar calendar = gc.qlCalendar;
    BusinessDayConvention bdc = gc.qlBdc;
    int maxPoints = (options.present && options.maxPoints > 0) ? options.maxPoints : 50000;

    if (gridSpec.kind == SampleDateGridKind::Tenor) {
        const auto& grid = gridSpec.tenor;
        if (!grid.hasTenors) {
            QUANTRA_INVALID_ARGUMENT("TenorGrid.tenors is required");
        }
        dates.reserve(grid.tenors.size());
        for (size_t i = 0; i < grid.tenors.size(); ++i) {
            const auto& t = grid.tenors[i];
            QuantLib::Period p(t.n, TimeUnitToQL(t.unit));
            if (p.length() == 0 && p.units() != Days) {
                QUANTRA_INVALID_ARGUMENT("TenorGrid only allows zero period as 0 Days");
            }
            Date d = calendar.advance(referenceDate, p, bdc);
            dates.push_back(d);
        }
    } else if (gridSpec.kind == SampleDateGridKind::Range) {
        const auto& grid = gridSpec.range;
        if (!grid.hasEndDate) {
            QUANTRA_INVALID_ARGUMENT("RangeGrid.end_date is required");
        }
        Date startDate = grid.hasStartDate ? DateToQL(grid.startDate) : asOfDate;
        Date endDate = DateToQL(grid.endDate);
        int stepNumber = std::max(1, grid.stepNumber);
        TimeUnit stepUnit = TimeUnitToQL(grid.stepTimeUnit);
        QuantLib::Period step(stepNumber, stepUnit);
        bool businessDaysOnly = grid.businessDaysOnly;

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

std::vector<QuantLib::Period> buildTenorPeriods(const SampleDateGridSpec& gridSpec) {
    if (!gridSpec.present || gridSpec.kind != SampleDateGridKind::Tenor) {
        QUANTRA_INVALID_ARGUMENT("Swaption tenor_grid must be a TenorGrid");
    }
    const auto& grid = gridSpec.tenor;
    if (!grid.hasTenors || grid.tenors.empty()) {
        QUANTRA_INVALID_ARGUMENT("Swaption tenor_grid.tenors is required");
    }
    std::vector<QuantLib::Period> periods;
    periods.reserve(grid.tenors.size());
    for (const auto& t : grid.tenors) {
        periods.emplace_back(t.n, TimeUnitToQL(t.unit));
    }
    return periods;
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

void checkPointBudget(int64_t points, const SampleQueryOptions& options) {
    int maxPoints = (options.present && options.maxPoints > 0) ? options.maxPoints : 50000;
    if (points > maxPoints) {
        QUANTRA_INVALID_ARGUMENT("Query exceeds max_points: " + std::to_string(points) +
                      " > " + std::to_string(maxPoints));
    }
}

VolSurfaceSampleResult priceOneQuery(
    const SampleVolSurfacesQuery& q,
    const PricingRegistry& reg,
    const Date& asOf) {
    VolSurfaceSampleResult sample;
    sample.volId = q.volId;

    const std::string& volId = q.volId;
    if (volId.empty()) {
        QUANTRA_INVALID_ARGUMENT("VolQuerySpec.vol_id is required");
    }
    if (!q.hasStrikeGrid || !q.hasStrikes || q.strikes.empty()) {
        QUANTRA_INVALID_ARGUMENT("VolQuerySpec.strike_grid.strikes is required");
    }

    std::vector<Date> expiriesOut;
    std::vector<Date> requestedExpiryGridOut;
    std::vector<QuantLib::Period> tenorsOut;
    std::vector<Date> effectiveSwapStartsOut;
    std::vector<Date> effectiveSwapEndsOut;
    std::vector<double> strikesOut;
    std::vector<double> volsOut;
    std::vector<double> atmLevelsOut;
    SampleExpiryKind expiryKindOut = SampleExpiryKind::GridDate;
    quantra::enums::VolatilityType volTypeOut = quantra::enums::VolatilityType_Lognormal;
    quantra::enums::SwaptionStrikeKind canonicalStrikeKind = quantra::enums::SwaptionStrikeKind_Absolute;
    SampleStrikeAxis requestedStrikeAxis = q.strikeAxis;
    Date sampleReferenceDate = asOf;
    quantra::enums::Calendar usedCalendar = quantra::enums::Calendar_NullCalendar;
    quantra::enums::BusinessDayConvention usedBdc = quantra::enums::BusinessDayConvention_Following;
    bool allowExtrapolationUsed = true;
    int nExpOut = 0, nTenOut = 0, nStrOut = 0;

    if (q.surfaceType == SampleSurfaceType::Swaption) {
        auto vIt = reg.volatility.swaptionVols.find(volId);
        if (vIt == reg.volatility.swaptionVols.end()) {
            QUANTRA_NOT_FOUND("Swaption vol not found: " + volId);
        }
        SwaptionVolEntry volEntry = vIt->second;
        if (volEntry.referenceDate == Date()) {
            QUANTRA_INVALID_ARGUMENT("Swaption vol has invalid referenceDate: " + volId);
        }
        const SampleQueryOptions& options = q.options;
        const bool strictMode = !options.present || options.strict;
        if (strictMode && volEntry.referenceDate != asOf) {
            std::ostringstream err;
            err << "Strict mode: pricing.as_of_date (" << DateToIso(asOf)
                << ") must equal swaption vol referenceDate ("
                << DateToIso(volEntry.referenceDate) << ") for vol '" << volId << "'";
            QUANTRA_INVALID_ARGUMENT(err.str());
        }
        if (!q.tenorGrid.present) {
            QUANTRA_INVALID_ARGUMENT("VolQuerySpec.tenor_grid is required for swaption sampling");
        }
        if (!q.expiryGrid.present) {
            QUANTRA_INVALID_ARGUMENT("VolQuerySpec.expiry_grid is required for swaption sampling");
        }
        canonicalStrikeKind = volEntry.strikeKind;
        sampleReferenceDate = volEntry.referenceDate;
        const bool allowExtrapolation = !options.present || options.allowExtrapolation;
        allowExtrapolationUsed = allowExtrapolation;

        if (volEntry.swapIndexId.empty()) {
            QUANTRA_INVALID_ARGUMENT("Swaption surface is missing required swap_index_id");
        }
        std::string swapIndexId = volEntry.swapIndexId;
        if (q.hasSwapIndexId && !q.swapIndexId.empty()) {
            swapIndexId = q.swapIndexId;
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
        if (q.expiryGrid.kind == SampleDateGridKind::Tenor) {
            auto expPeriods = buildTenorPeriods(q.expiryGrid);
            rawExpiryGrid.reserve(expPeriods.size());
            expiries.reserve(expPeriods.size());
            for (const auto& p : expPeriods) {
                Date gridDate = sidx.fixedCalendar.advance(volEntry.referenceDate, p, sidx.fixedBdc);
                rawExpiryGrid.push_back(gridDate);
                expiries.push_back(gridDate);
            }
        } else {
            rawExpiryGrid = buildDateGrid(
                q.expiryGrid, volEntry.referenceDate, asOf, options,
                sidx.fixedCalendar, sidx.fixedCalendarFb, sidx.fixedBdc, sidx.fixedBdcFb);
            expiries.reserve(rawExpiryGrid.size());
            for (const auto& d : rawExpiryGrid) {
                expiries.push_back(sidx.fixedCalendar.adjust(d, sidx.fixedBdc));
            }
        }
        for (const auto& d : rawExpiryGrid) {
            requestedExpiryGridOut.push_back(d);
        }
        std::vector<QuantLib::Period> tenors = buildTenorPeriods(q.tenorGrid);
        std::vector<double> strikes;
        strikes.reserve(q.strikes.size());
        for (size_t i = 0; i < q.strikes.size(); ++i) {
            strikes.push_back(q.strikes[i]);
        }
        validateStrictlyIncreasingDates(expiries, "expiry_grid (after swap-index adjustment)");
        validateStrictlyIncreasingStrikes(strikes);

        SampleStrikeAxis axis = requestedStrikeAxis;
        if (volEntry.strikeKind == quantra::enums::SwaptionStrikeKind_Absolute &&
            axis == SampleStrikeAxis::SpreadFromATM) {
            QUANTRA_INVALID_ARGUMENT("SpreadFromATM strike axis requested for Absolute-strike swaption vol");
        }

        // SpreadFromATM smile cubes and SABR-params surfaces both need
        // forward resolution before sampling: SpreadFromATM to translate
        // strike spreads to absolute strikes, SABR to instantiate per-node
        // SabrSmileSection from F(expiry, tenor). Both go through the
        // shared finalizeSwaptionVolEntryForPricing path.
        const bool needsForwardResolution =
            volEntry.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM ||
            volEntry.volKind == quantra::enums::SwaptionVolKind_SabrParams ||
            volEntry.volKind == quantra::enums::SwaptionVolKind_SabrCalibrate;
        if (needsForwardResolution) {
            if (!q.hasDiscountingCurveId || q.discountingCurveId.empty() ||
                !q.hasForwardingCurveId || q.forwardingCurveId.empty()) {
                QUANTRA_INVALID_ARGUMENT(
                    "SpreadFromATM/SABR swaption sampling requires "
                    "discounting_curve_id and forwarding_curve_id");
            }
            auto dIt = reg.rates.curves.find(q.discountingCurveId);
            auto fIt = reg.rates.curves.find(q.forwardingCurveId);
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
                q.discountingCurveId,
                q.forwardingCurveId);
        }

        if (volEntry.handle.empty()) {
            QUANTRA_ERROR("Swaption vol handle is empty");
        }
        if (allowExtrapolation) volEntry.handle->enableExtrapolation();
        else volEntry.handle->disableExtrapolation();

        int64_t nExp = static_cast<int64_t>(expiries.size());
        int64_t nTen = static_cast<int64_t>(tenors.size());
        int64_t nStr = static_cast<int64_t>(strikes.size());
        SampleOutputMode mode = q.outputMode;
        if (mode == SampleOutputMode::Cube) checkPointBudget(nExp * nTen * nStr, options);
        else if (mode == SampleOutputMode::SmileSlice) checkPointBudget(nStr, options);
        else if (mode == SampleOutputMode::TermSlice) checkPointBudget(nTen, options);
        else if (mode == SampleOutputMode::ExpirySlice) checkPointBudget(nExp, options);

        const Date evalDate = Settings::instance().evaluationDate();
        int expIdx = 0;
        int tenIdx = 0;

        std::vector<double> precomputedAtm;
        struct SwaptionNodeDates {
            Date exercise;
            Date start;
            Date end;
        };

        auto requireSelectors = [&](SampleOutputMode m) {
            if (m == SampleOutputMode::SmileSlice) {
                expIdx = resolveSelectorIndex(q.sliceExpiryIndex, static_cast<int>(expiries.size()), "slice_expiry_index", true);
                tenIdx = resolveSelectorIndex(q.sliceTenorIndex, static_cast<int>(tenors.size()), "slice_tenor_index", true);
            } else if (m == SampleOutputMode::TermSlice) {
                expIdx = resolveSelectorIndex(q.sliceExpiryIndex, static_cast<int>(expiries.size()), "slice_expiry_index", true);
                if (!q.sliceStrikeIsSet) {
                    QUANTRA_INVALID_ARGUMENT("TermSlice requires slice_strike_is_set=true");
                }
                if (!std::isfinite(q.sliceStrike)) {
                    QUANTRA_INVALID_ARGUMENT("TermSlice requires finite slice_strike");
                }
            } else if (m == SampleOutputMode::ExpirySlice) {
                tenIdx = resolveSelectorIndex(q.sliceTenorIndex, static_cast<int>(tenors.size()), "slice_tenor_index", true);
                if (!q.sliceStrikeIsSet) {
                    QUANTRA_INVALID_ARGUMENT("ExpirySlice requires slice_strike_is_set=true");
                }
                if (!std::isfinite(q.sliceStrike)) {
                    QUANTRA_INVALID_ARGUMENT("ExpirySlice requires finite slice_strike");
                }
            }
        };
        requireSelectors(mode);

        auto atmLookup = [&](int iExp, int iTen) -> double {
            if (precomputedAtm.empty()) return std::numeric_limits<double>::quiet_NaN();
            if (mode == SampleOutputMode::Cube) {
                return precomputedAtm[static_cast<size_t>(iExp) * tenors.size() + static_cast<size_t>(iTen)];
            }
            if (mode == SampleOutputMode::SmileSlice) {
                return precomputedAtm[0];
            }
            if (mode == SampleOutputMode::TermSlice) {
                return precomputedAtm[static_cast<size_t>(iTen)];
            }
            // ExpirySlice
            return precomputedAtm[static_cast<size_t>(iExp)];
        };

        if (volEntry.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM) {
            if (!q.hasDiscountingCurveId || q.discountingCurveId.empty() ||
                !q.hasForwardingCurveId || q.forwardingCurveId.empty()) {
                QUANTRA_INVALID_ARGUMENT("SpreadFromATM requires discounting_curve_id/forwarding_curve_id");
            }
            auto dIt = reg.rates.curves.find(q.discountingCurveId);
            auto fIt = reg.rates.curves.find(q.forwardingCurveId);
            if (dIt == reg.rates.curves.end() || fIt == reg.rates.curves.end()) {
                QUANTRA_NOT_FOUND("Sampling curve ids not found for ATM computation");
            }
            std::vector<Date> atmExpiries;
            std::vector<QuantLib::Period> atmTenors;
            if (mode == SampleOutputMode::Cube) {
                atmExpiries = expiries;
                atmTenors = tenors;
            } else if (mode == SampleOutputMode::SmileSlice) {
                atmExpiries = {expiries[static_cast<size_t>(expIdx)]};
                atmTenors = {tenors[static_cast<size_t>(tenIdx)]};
            } else if (mode == SampleOutputMode::TermSlice) {
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
            if (volEntry.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM) {
                atm = atmLookup(iExp, iTen);
            }

            double absStrike = strikeInput;
            double spread = strikeInput;
            if (volEntry.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM) {
                if (axis == SampleStrikeAxis::SpreadFromATM) {
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

            if (!allowExtrapolation && volEntry.volKind != quantra::enums::SwaptionVolKind_Constant) {
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
            expiriesOut.push_back(d);
        };

        if (mode == SampleOutputMode::Cube) {
            for (size_t i = 0; i < expiries.size(); ++i) addExpiryLabel(expiries[i]);
            for (size_t j = 0; j < tenors.size(); ++j) tenorsOut.push_back(tenors[j]);
            strikesOut = strikes;
            expiryKindOut = SampleExpiryKind::ExerciseDate;

            for (size_t i = 0; i < expiries.size(); ++i) {
                for (size_t j = 0; j < tenors.size(); ++j) {
                    auto dates = computeSwaptionDates(static_cast<int>(i), static_cast<int>(j));
                    std::pair<double, double> tenorBounds;
                    bool enforceTenorBounds = (!allowExtrapolation && volEntry.volKind != quantra::enums::SwaptionVolKind_Constant);
                    if (enforceTenorBounds) {
                        tenorBounds = computeSwapLengthSupportBounds(dates);
                        double swapLength = std::max(1.0e-8, volEntry.dayCounter.yearFraction(dates.start, dates.end));
                        checkTenorSupport(swapLength, tenorBounds);
                    }
                    effectiveSwapStartsOut.push_back(dates.start);
                    effectiveSwapEndsOut.push_back(dates.end);
                    if (volEntry.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM &&
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
        } else if (mode == SampleOutputMode::SmileSlice) {
            int i = expIdx;
            int j = tenIdx;
            addExpiryLabel(expiries[static_cast<size_t>(i)]);
            tenorsOut.push_back(tenors[static_cast<size_t>(j)]);
            strikesOut = strikes;
            expiryKindOut = SampleExpiryKind::ExerciseDate;
            auto dates = computeSwaptionDates(i, j);
            std::pair<double, double> tenorBounds;
            if (!allowExtrapolation && volEntry.volKind != quantra::enums::SwaptionVolKind_Constant) {
                tenorBounds = computeSwapLengthSupportBounds(dates);
                double swapLength = std::max(1.0e-8, volEntry.dayCounter.yearFraction(dates.start, dates.end));
                checkTenorSupport(swapLength, tenorBounds);
            }
            effectiveSwapStartsOut.push_back(dates.start);
            effectiveSwapEndsOut.push_back(dates.end);
            if (volEntry.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM) {
                double atm = atmLookup(i, j);
                if (std::isfinite(atm)) atmLevelsOut.push_back(atm);
            }
            for (double strike : strikes) {
                volsOut.push_back(sampleVol(i, j, strike, dates));
            }
            nExpOut = 1; nTenOut = 1; nStrOut = static_cast<int>(strikes.size());
        } else if (mode == SampleOutputMode::TermSlice) {
            int i = expIdx;
            addExpiryLabel(expiries[static_cast<size_t>(i)]);
            strikesOut.push_back(q.sliceStrike);
            expiryKindOut = SampleExpiryKind::ExerciseDate;
            for (size_t j = 0; j < tenors.size(); ++j) {
                auto dates = computeSwaptionDates(i, static_cast<int>(j));
                std::pair<double, double> tenorBounds;
                if (!allowExtrapolation && volEntry.volKind != quantra::enums::SwaptionVolKind_Constant) {
                    tenorBounds = computeSwapLengthSupportBounds(dates);
                    double swapLength = std::max(1.0e-8, volEntry.dayCounter.yearFraction(dates.start, dates.end));
                    checkTenorSupport(swapLength, tenorBounds);
                }
                volsOut.push_back(sampleVol(i, static_cast<int>(j), q.sliceStrike, dates));
                tenorsOut.push_back(tenors[j]);
                effectiveSwapStartsOut.push_back(dates.start);
                effectiveSwapEndsOut.push_back(dates.end);
                if (volEntry.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM) {
                    double atm = atmLookup(i, static_cast<int>(j));
                    if (std::isfinite(atm)) atmLevelsOut.push_back(atm);
                }
            }
            nExpOut = 1; nTenOut = static_cast<int>(tenors.size()); nStrOut = 1;
        } else if (mode == SampleOutputMode::ExpirySlice) {
            int j = tenIdx;
            strikesOut.push_back(q.sliceStrike);
            tenorsOut.push_back(tenors[static_cast<size_t>(j)]);
            expiryKindOut = SampleExpiryKind::ExerciseDate;
            for (size_t i = 0; i < expiries.size(); ++i) {
                addExpiryLabel(expiries[i]);
                auto dates = computeSwaptionDates(static_cast<int>(i), j);
                std::pair<double, double> tenorBounds;
                if (!allowExtrapolation && volEntry.volKind != quantra::enums::SwaptionVolKind_Constant) {
                    tenorBounds = computeSwapLengthSupportBounds(dates);
                    double swapLength = std::max(1.0e-8, volEntry.dayCounter.yearFraction(dates.start, dates.end));
                    checkTenorSupport(swapLength, tenorBounds);
                }
                volsOut.push_back(sampleVol(static_cast<int>(i), j, q.sliceStrike, dates));
                effectiveSwapStartsOut.push_back(dates.start);
                effectiveSwapEndsOut.push_back(dates.end);
                if (!precomputedAtm.empty()) {
                    double atm = atmLookup(static_cast<int>(i), j);
                    if (std::isfinite(atm)) atmLevelsOut.push_back(atm);
                }
            }
            nExpOut = static_cast<int>(expiries.size()); nTenOut = 1; nStrOut = 1;
        } else {
            QUANTRA_INVALID_ARGUMENT("Unsupported VolOutputMode");
        }

        volTypeOut = VolatilityTypeToFb(volEntry.qlVolType, volEntry.displacement);
    } else if (q.surfaceType == SampleSurfaceType::EquityBlack) {
        auto vIt = reg.volatility.blackVols.find(volId);
        if (vIt == reg.volatility.blackVols.end()) {
            QUANTRA_NOT_FOUND("Black vol not found: " + volId);
        }
        const BlackVolEntry& volEntry = vIt->second;

        if (!q.expiryGrid.present) {
            QUANTRA_INVALID_ARGUMENT("VolQuerySpec.expiry_grid is required for equity black sampling");
        }
        if (q.outputMode != SampleOutputMode::Cube) {
            QUANTRA_INVALID_ARGUMENT("EquityBlack sampling supports Cube output_mode only");
        }
        if (q.strikeAxis == SampleStrikeAxis::SpreadFromATM) {
            QUANTRA_INVALID_ARGUMENT("EquityBlack sampling supports AbsoluteStrike axis only");
        }
        if (q.tenorGrid.present) {
            QUANTRA_INVALID_ARGUMENT("tenor_grid is not valid for equity black sampling");
        }
        if (q.hasSwapIndexId && !q.swapIndexId.empty()) {
            QUANTRA_INVALID_ARGUMENT("swap_index_id is not valid for equity black sampling");
        }
        if ((q.hasDiscountingCurveId && !q.discountingCurveId.empty()) ||
            (q.hasForwardingCurveId && !q.forwardingCurveId.empty())) {
            QUANTRA_INVALID_ARGUMENT(
                "discounting_curve_id/forwarding_curve_id are not valid for equity black sampling");
        }
        if (q.sliceExpiryIndex >= 0 || q.sliceTenorIndex >= 0 || q.sliceStrikeIsSet) {
            QUANTRA_INVALID_ARGUMENT("EquityBlack query does not support slice selectors");
        }
        const SampleQueryOptions& options = q.options;
        const bool strictMode = !options.present || options.strict;
        if (strictMode && volEntry.referenceDate != asOf) {
            std::ostringstream err;
            err << "Strict mode: pricing.as_of_date (" << DateToIso(asOf)
                << ") must equal equity black vol referenceDate ("
                << DateToIso(volEntry.referenceDate) << ") for vol '" << volId << "'";
            QUANTRA_INVALID_ARGUMENT(err.str());
        }
        sampleReferenceDate = volEntry.referenceDate;
        const bool allowExtrapolation = !options.present || options.allowExtrapolation;
        allowExtrapolationUsed = allowExtrapolation;
        canonicalStrikeKind = quantra::enums::SwaptionStrikeKind_Absolute;
        expiryKindOut = SampleExpiryKind::GridDate;
        volTypeOut = quantra::enums::VolatilityType_Lognormal;

        GridConventions gc = resolveGridConventions(
            q.expiryGrid, options, volEntry.calendar, volEntry.calendarFb,
            volEntry.businessDayConvention, volEntry.businessDayConventionFb);
        usedCalendar = gc.fbCalendar;
        usedBdc = gc.fbBdc;

        if (allowExtrapolation) volEntry.handle->enableExtrapolation();
        else volEntry.handle->disableExtrapolation();

        std::vector<Date> expiries = buildDateGrid(
            q.expiryGrid, sampleReferenceDate, asOf, options,
            volEntry.calendar, gc.fbCalendar,
            volEntry.businessDayConvention, volEntry.businessDayConventionFb);
        validateStrictlyIncreasingDates(expiries, "expiry_grid");
        for (const auto& d : expiries) {
            expiriesOut.push_back(d);
        }

        for (size_t i = 0; i < q.strikes.size(); ++i) {
            strikesOut.push_back(q.strikes[i]);
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
    } else if (q.surfaceType == SampleSurfaceType::Optionlet) {
        auto vIt = reg.volatility.optionletVols.find(volId);
        if (vIt == reg.volatility.optionletVols.end()) {
            QUANTRA_NOT_FOUND("Optionlet vol not found: " + volId);
        }
        const OptionletVolEntry& volEntry = vIt->second;
        const SampleQueryOptions& options = q.options;
        const bool strictMode = !options.present || options.strict;
        if (strictMode && volEntry.referenceDate != asOf) {
            std::ostringstream err;
            err << "Strict mode: pricing.as_of_date (" << DateToIso(asOf)
                << ") must equal optionlet vol referenceDate ("
                << DateToIso(volEntry.referenceDate) << ") for vol '" << volId << "'";
            QUANTRA_INVALID_ARGUMENT(err.str());
        }
        if (q.strikeAxis == SampleStrikeAxis::SpreadFromATM) {
            QUANTRA_INVALID_ARGUMENT("Optionlet sampling supports AbsoluteStrike axis only");
        }
        if (q.outputMode != SampleOutputMode::Cube) {
            QUANTRA_INVALID_ARGUMENT("Optionlet sampling supports Cube output_mode only");
        }
        if (q.hasSwapIndexId && !q.swapIndexId.empty()) {
            QUANTRA_INVALID_ARGUMENT("swap_index_id is not valid for optionlet sampling");
        }
        if (q.sliceExpiryIndex >= 0 || q.sliceTenorIndex >= 0 || q.sliceStrikeIsSet) {
            QUANTRA_INVALID_ARGUMENT("Optionlet query does not support slice selectors");
        }
        const bool allowExtrapolation = !options.present || options.allowExtrapolation;
        sampleReferenceDate = volEntry.referenceDate;
        canonicalStrikeKind = quantra::enums::SwaptionStrikeKind_Absolute;
        GridConventions gc = resolveGridConventions(
            q.expiryGrid, options, volEntry.calendar, volEntry.calendarFb,
            volEntry.businessDayConvention, volEntry.businessDayConventionFb);
        usedCalendar = gc.fbCalendar;
        usedBdc = gc.fbBdc;
        allowExtrapolationUsed = allowExtrapolation;
        expiryKindOut = SampleExpiryKind::GridDate;
        if (allowExtrapolation) volEntry.handle->enableExtrapolation();
        else volEntry.handle->disableExtrapolation();
        std::vector<Date> expiries = buildDateGrid(
            q.expiryGrid, volEntry.referenceDate, asOf, options,
            volEntry.calendar, gc.fbCalendar,
            volEntry.businessDayConvention, volEntry.businessDayConventionFb);
        validateStrictlyIncreasingDates(expiries, "expiry_grid");
        for (size_t i = 0; i < expiries.size(); ++i) {
            expiriesOut.push_back(expiries[i]);
        }
        for (size_t i = 0; i < q.strikes.size(); ++i) {
            strikesOut.push_back(q.strikes[i]);
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
        volTypeOut = VolatilityTypeToFb(volEntry.qlVolType, volEntry.displacement);
    } else {
        QUANTRA_INVALID_ARGUMENT("Unknown VolSurfaceType");
    }

    sample.referenceDate = sampleReferenceDate;
    sample.qlVolType = volTypeOut;
    sample.requestedStrikeAxis = requestedStrikeAxis;
    sample.canonicalStrikeKind = canonicalStrikeKind;
    sample.allowExtrapolationUsed = allowExtrapolationUsed;
    sample.calendarUsed = usedCalendar;
    sample.businessDayConventionUsed = usedBdc;
    sample.expiryKind = expiryKindOut;
    sample.expiries = std::move(expiriesOut);
    sample.requestedExpiryGridPoints = std::move(requestedExpiryGridOut);
    sample.tenors = std::move(tenorsOut);
    sample.effectiveSwapStarts = std::move(effectiveSwapStartsOut);
    sample.effectiveSwapEnds = std::move(effectiveSwapEndsOut);
    sample.strikes = std::move(strikesOut);
    sample.vols = std::move(volsOut);
    sample.nExpiries = nExpOut;
    sample.nTenors = nTenOut;
    sample.nStrikes = nStrOut;
    sample.atmLevels = std::move(atmLevelsOut);
    return sample;
}

} // namespace

SampleVolSurfacesResult SampleVolSurfacesPricer::price(
    const SampleVolSurfacesInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    const Date asOf = ctx.asOf;

    SampleVolSurfacesResult result;
    for (const auto& q : inputs.queries) {
        if (!q.prebuiltError.empty()) {
            VolSurfaceSampleResult err;
            err.volId = q.volId;
            err.hasError = true;
            err.error = q.prebuiltError;
            result.samples.push_back(std::move(err));
            continue;
        }
        try {
            result.samples.push_back(priceOneQuery(q, reg, asOf));
        } catch (const std::exception& e) {
            VolSurfaceSampleResult err;
            err.volId = q.volId;
            err.hasError = true;
            err.error = e.what();
            result.samples.push_back(std::move(err));
        }
    }

    if (inputs.includeDiagnostics) {
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
        for (const auto& q : inputs.queries) {
            const std::string& vid = q.volId;
            if (vid.empty()) continue;
            auto it = reg.volatility.swaptionVols.find(vid);
            if (it == reg.volatility.swaptionVols.end()) continue;
            const auto& e = it->second;
            if (e.volKind != quantra::enums::SwaptionVolKind_SabrCalibrate &&
                e.volKind != quantra::enums::SwaptionVolKind_SabrParams) {
                continue;
            }
            if (seen.count(vid)) continue;
            seen[vid] = static_cast<int>(jobs.size());
            DiagJob j;
            j.volId = vid;
            if (q.hasDiscountingCurveId) j.discountCurveId = q.discountingCurveId;
            if (q.hasForwardingCurveId) j.forwardingCurveId = q.forwardingCurveId;
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
            SampleVolDiagnostic diag;
            diag.volId = job.volId;
            diag.warnings = std::move(warnings);
            if (ok) {
                diag.partial = false;
                diag.finalized = std::move(entry);
            } else {
                diag.partial = true;
                diag.kind = it->second.volKind;
            }
            result.diagnostics.push_back(std::move(diag));
        }
    }

    return result;
}

} // namespace quantra
