#include "inflation_curve_parsers.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <set>
#include <unordered_map>

#include <ql/indexes/inflationindex.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/inflation/inflationhelpers.hpp>
#include <ql/termstructures/inflation/piecewisezeroinflationcurve.hpp>
#include <ql/termstructures/inflation/piecewiseyoyinflationcurve.hpp>
#include <ql/currencies/all.hpp>
#include <ql/currencies/europe.hpp>
#include <ql/currencies/america.hpp>
#include <ql/currencies/asia.hpp>
#include <ql/currencies/oceania.hpp>
#include <ql/time/period.hpp>
#include <ql/time/schedule.hpp>

#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"
#include "require_scalar.h"
#include "index_registry_builder.h" // CurrencyFromString

namespace quantra {

namespace {

const quantra::InflationIndexSpec* findInflationIndexSpec(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::InflationIndexSpec>>* indices,
    const std::string& id) {
    if (!indices) return nullptr;
    for (auto it = indices->begin(); it != indices->end(); ++it) {
        const auto* s = *it;
        if (s && s->id() && s->id()->str() == id) return s;
    }
    return nullptr;
}

std::unordered_map<std::string, const quantra::InflationIndexSpec*> buildInflationIndexSpecMap(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::InflationIndexSpec>>* indices) {
    std::unordered_map<std::string, const quantra::InflationIndexSpec*> out;
    if (!indices) {
        return out;
    }
    for (auto it = indices->begin(); it != indices->end(); ++it) {
        const auto* spec = *it;
        if (spec && spec->id()) {
            out.emplace(spec->id()->str(), spec);
        }
    }
    return out;
}

QuantLib::Period toQlPeriodReq(const quantra::Period* p, const std::string& label) {
    if (!p) {
        QUANTRA_INVALID_ARGUMENT(label + " is required");
    }
    return QuantLib::Period(p->n(), TimeUnitToQL(p->unit()));
}

QuantLib::CPI::InterpolationType cpiInterpolationToQL(quantra::enums::CPIInterpolationType interpolation) {
    switch (interpolation) {
        case quantra::enums::CPIInterpolationType_AsIndex:
            return QuantLib::CPI::AsIndex;
        case quantra::enums::CPIInterpolationType_Flat:
            return QuantLib::CPI::Flat;
        case quantra::enums::CPIInterpolationType_Linear:
            return QuantLib::CPI::Linear;
    }
    QUANTRA_INVALID_ARGUMENT("Unsupported CPIInterpolationType");
    return QuantLib::CPI::AsIndex;
}

void validateInterpolator(enums::Interpolator interpolator, const std::string& curveId) {
    if (interpolator != enums::Interpolator_Linear) {
        QUANTRA_INVALID_ARGUMENT("Inflation curves only support Linear interpolator for curve id: " + curveId);
    }
}

template <typename THelperSpec>
double resolveQuoteOrInline(
    const THelperSpec* helper,
    const QuoteRegistry* quotes,
    const std::string& curveId,
    const std::string& helperLabel) {
    if (!helper) {
        QUANTRA_INVALID_ARGUMENT(helperLabel + " missing for curve id: " + curveId);
    }
    const bool hasQuoteId = helper->quote_id() && !helper->quote_id()->str().empty();
    if (hasQuoteId) {
        if (!quotes) {
            QUANTRA_INVALID_ARGUMENT(helperLabel + ".quote_id requires QuoteRegistry for curve id: " + curveId);
        }
        return quotes->getValue(helper->quote_id()->str(), quantra::QuoteType_Curve);
    }
    return requireFinite(helper->quote_value(), helperLabel + ".quote_value");
}

QuantLib::Date parseDateReq(const flatbuffers::String* s, const std::string& label) {
    if (!s || s->str().empty()) {
        QUANTRA_INVALID_ARGUMENT(label + " is required");
    }
    return DateToQL(s->str());
}

QuantLib::Region regionFromCurrency(const std::string& ccy) {
    if (ccy == "EUR") return QuantLib::EURegion();
    if (ccy == "USD") return QuantLib::USRegion();
    if (ccy == "GBP") return QuantLib::UKRegion();
    if (ccy == "AUD") return QuantLib::AustraliaRegion();
    if (ccy == "ZAR") return QuantLib::ZARegion();
    return QuantLib::CustomRegion("N/A", "N/A");
}

template <typename TIndex>
void applyFixings(
    const QuantLib::ext::shared_ptr<TIndex>& index,
    const flatbuffers::Vector<flatbuffers::Offset<quantra::Fixing>>* fixings,
    const std::string& label) {
    if (!fixings) {
        return;
    }
    index->clearFixings();
    for (auto it = fixings->begin(); it != fixings->end(); ++it) {
        const auto* fixing = *it;
        if (!fixing || !fixing->date()) {
            QUANTRA_INVALID_ARGUMENT(label + ".fixings[].date is required");
        }
        index->addFixing(DateToQL(fixing->date()->str()), fixing->value());
    }
}

bool hasText(const flatbuffers::String* s) {
    return s && !s->str().empty();
}

bool hasFixingForDate(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::Fixing>>* fixings,
    const QuantLib::Date& target) {
    if (!fixings) {
        return false;
    }
    for (auto it = fixings->begin(); it != fixings->end(); ++it) {
        const auto* fixing = *it;
        if (fixing && fixing->date() && DateToQL(fixing->date()->str()) == target) {
            return true;
        }
    }
    return false;
}

struct HelperDates {
    bool useExplicitStartEnd = false;
    QuantLib::Date startDate;
    QuantLib::Date endDate;
};

template <typename THelperSpec>
HelperDates resolveHelperDates(
    const THelperSpec* helper,
    const QuantLib::Date& referenceDate,
    const QuantLib::Calendar& calendar,
    QuantLib::BusinessDayConvention businessDayConvention,
    const std::string& curveId,
    const std::string& helperLabel) {
    const bool hasTenor = helper->tenor() != nullptr;
    const bool hasStart = hasText(helper->start_date());
    const bool hasEnd = hasText(helper->end_date());
    if (hasStart && !hasEnd) {
        QUANTRA_INVALID_ARGUMENT(helperLabel + " requires end_date when start_date is provided for curve id: " + curveId);
    }
    if (hasTenor && (hasStart || hasEnd)) {
        QUANTRA_INVALID_ARGUMENT(helperLabel + " cannot specify both tenor and explicit dates for curve id: " + curveId);
    }

    HelperDates out;
    if (hasStart) {
        out.useExplicitStartEnd = true;
        out.startDate = DateToQL(helper->start_date()->str());
        out.endDate = DateToQL(helper->end_date()->str());
        if (out.endDate <= out.startDate) {
            QUANTRA_INVALID_ARGUMENT(helperLabel + ".end_date must be after start_date for curve id: " + curveId);
        }
        return out;
    }

    if (!hasTenor && !hasEnd) {
        QUANTRA_INVALID_ARGUMENT(helperLabel + " requires tenor or end_date for curve id: " + curveId);
    }
    if (hasEnd) {
        out.endDate = DateToQL(helper->end_date()->str());
    } else {
        const QuantLib::Period tenor = toQlPeriodReq(helper->tenor(), helperLabel + ".tenor");
        out.endDate = calendar.advance(referenceDate, tenor, businessDayConvention);
    }
    if (out.endDate <= referenceDate) {
        QUANTRA_INVALID_ARGUMENT(helperLabel + " maturity must be after reference_date for curve id: " + curveId);
    }
    return out;
}

QuantLib::Schedule buildSchedule(
    const quantra::Schedule* scheduleSpec,
    const std::string& curveId,
    const std::string& helperLabel) {
    if (!scheduleSpec) {
        QUANTRA_INVALID_ARGUMENT(helperLabel + ".schedule is required for curve id: " + curveId);
    }
    if (!scheduleSpec->effective_date() || !scheduleSpec->termination_date()) {
        QUANTRA_INVALID_ARGUMENT(helperLabel + ".schedule requires effective_date and termination_date for curve id: " + curveId);
    }
    // The convention enums are presence-required: an omitted convention is an
    // error, never an alphabetical-0 default (calendar Argentina, etc.).
    if (!scheduleSpec->calendar().has_value() ||
        !scheduleSpec->frequency().has_value() ||
        !scheduleSpec->convention().has_value() ||
        !scheduleSpec->termination_date_convention().has_value() ||
        !scheduleSpec->date_generation_rule().has_value()) {
        QUANTRA_INVALID_ARGUMENT(helperLabel + ".schedule requires calendar, frequency, convention, termination_date_convention and date_generation_rule for curve id: " + curveId);
    }
    return QuantLib::Schedule(
        DateToQL(scheduleSpec->effective_date()->str()),
        DateToQL(scheduleSpec->termination_date()->str()),
        FrequencyToPeriod(FrequencyToQL(scheduleSpec->frequency().value())),
        CalendarToQL(scheduleSpec->calendar().value()),
        ConventionToQL(scheduleSpec->convention().value()),
        ConventionToQL(scheduleSpec->termination_date_convention().value()),
        DateGenerationToQL(scheduleSpec->date_generation_rule().value()),
        scheduleSpec->end_of_month());
}

template <typename TTermStructure>
QuantLib::Handle<TTermStructure> lookupNominalCurveHandle(
    const std::string& curveId,
    const std::string& nominalCurveId,
    const PricingRegistry& reg,
    const std::string& helperLabel) {
    auto it = reg.rates.curves.find(nominalCurveId);
    if (it == reg.rates.curves.end() || !it->second || it->second->empty()) {
        QUANTRA_NOT_FOUND(helperLabel + " nominal_curve_id not found for curve id '" + curveId + "': " + nominalCurveId);
    }
    return QuantLib::Handle<TTermStructure>(it->second->currentLink());
}

using InflationIndexPtr = QuantLib::ext::shared_ptr<QuantLib::InflationIndex>;

InflationIndexPtr buildInflationIndex(
    const quantra::InflationIndexSpec* spec,
    const std::unordered_map<std::string, const quantra::InflationIndexSpec*>& specMap,
    std::unordered_map<std::string, InflationIndexPtr>& cache,
    const std::unordered_map<std::string, QuantLib::Handle<QuantLib::ZeroInflationTermStructure>>& zeroCurveHandles,
    const std::unordered_map<std::string, QuantLib::Handle<QuantLib::YoYInflationTermStructure>>& yoyCurveHandles) {
    if (!spec || !spec->id()) {
        QUANTRA_INVALID_ARGUMENT("InflationIndexSpec.id is required");
    }
    const std::string id = spec->id()->str();
    auto cached = cache.find(id);
    if (cached != cache.end()) {
        return cached->second;
    }

    const std::string family = hasText(spec->family_name()) ? spec->family_name()->str() : id;
    const std::string ccyStr = hasText(spec->currency()) ? spec->currency()->str() : "EUR";
    QuantLib::Currency ccy = CurrencyFromString(ccyStr);
    QuantLib::Region region = regionFromCurrency(ccyStr);
    QuantLib::Frequency freq = FrequencyToQL(spec->frequency());
    QuantLib::Period availLag = toQlPeriodReq(spec->availability_lag(), "InflationIndexSpec.availability_lag");

    if (spec->kind() == enums::InflationCurveKind_ZeroInflation) {
        auto curveIt = zeroCurveHandles.find(id);
        QuantLib::Handle<QuantLib::ZeroInflationTermStructure> handle =
            curveIt != zeroCurveHandles.end() ? curveIt->second : QuantLib::Handle<QuantLib::ZeroInflationTermStructure>();
        auto index = QuantLib::ext::make_shared<QuantLib::ZeroInflationIndex>(
            family,
            region,
            spec->revised(),
            freq,
            availLag,
            ccy,
            handle);
        applyFixings(index, spec->fixings(), "InflationIndexSpec");
        cache.emplace(id, index);
        return index;
    }

    auto yoyCurveIt = yoyCurveHandles.find(id);
    QuantLib::Handle<QuantLib::YoYInflationTermStructure> yoyHandle =
        yoyCurveIt != yoyCurveHandles.end() ? yoyCurveIt->second : QuantLib::Handle<QuantLib::YoYInflationTermStructure>();

    QuantLib::ext::shared_ptr<QuantLib::YoYInflationIndex> yoyIndex;
    if (hasText(spec->underlying_zero_index_id())) {
        const std::string underlyingId = spec->underlying_zero_index_id()->str();
        auto underlyingIt = specMap.find(underlyingId);
        if (underlyingIt == specMap.end()) {
            QUANTRA_NOT_FOUND("InflationIndexSpec.underlying_zero_index_id not found for index id '" + id + "': " + underlyingId);
        }
        auto underlying = buildInflationIndex(underlyingIt->second, specMap, cache, zeroCurveHandles, yoyCurveHandles);
        auto zeroUnderlying = QuantLib::ext::dynamic_pointer_cast<QuantLib::ZeroInflationIndex>(underlying);
        if (!zeroUnderlying) {
            QUANTRA_INVALID_ARGUMENT("InflationIndexSpec.underlying_zero_index_id must reference a zero inflation index for index id: " + id);
        }
        if (spec->fixings() && spec->fixings()->size() > 0) {
            QUANTRA_INVALID_ARGUMENT("Ratio-based YoY inflation indices cannot define direct fixings for index id: " + id);
        }
        yoyIndex = QuantLib::ext::make_shared<QuantLib::YoYInflationIndex>(zeroUnderlying, yoyHandle);
    } else {
        yoyIndex = QuantLib::ext::make_shared<QuantLib::YoYInflationIndex>(
            family,
            region,
            spec->revised(),
            freq,
            availLag,
            ccy,
            yoyHandle);
        applyFixings(yoyIndex, spec->fixings(), "InflationIndexSpec");
    }

    cache.emplace(id, yoyIndex);
    return yoyIndex;
}

template <typename THelperPtr>
std::vector<QuantLib::Date> collectPillarDates(const std::vector<THelperPtr>& helpers) {
    std::set<QuantLib::Date> pillarDates;
    for (const auto& helper : helpers) {
        pillarDates.insert(helper->pillarDate());
    }
    return std::vector<QuantLib::Date>(pillarDates.begin(), pillarDates.end());
}

} // namespace

std::map<std::string, InflationCurveEntry> buildInflationCurves(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::InflationCurveSpec>>* curves,
    const flatbuffers::Vector<flatbuffers::Offset<quantra::InflationIndexSpec>>* indices,
    const QuoteRegistry* quotes,
    PricingRegistry& reg) {
    std::map<std::string, InflationCurveEntry> out;
    if (!curves) return out;

    const auto specMap = buildInflationIndexSpecMap(indices);

    for (auto it = curves->begin(); it != curves->end(); ++it) {
        const auto* spec = *it;
        if (!spec || !spec->id()) {
            QUANTRA_INVALID_ARGUMENT("InflationCurveSpec.id is required");
        }
        const std::string id = spec->id()->str();
        if (!spec->reference_date()) {
            QUANTRA_INVALID_ARGUMENT("InflationCurveSpec.reference_date is required for curve id: " + id);
        }
        if (!spec->index_id() || spec->index_id()->str().empty()) {
            QUANTRA_INVALID_ARGUMENT("InflationCurveSpec.index_id is required for curve id: " + id);
        }
        if (!spec->points() || spec->points()->size() == 0) {
            QUANTRA_INVALID_ARGUMENT("InflationCurveSpec.points are required for curve id: " + id);
        }
        validateInterpolator(spec->interpolator(), id);

        const std::string indexId = spec->index_id()->str();
        std::string discountCurveId;
        if (spec->discount_curve_id() && !spec->discount_curve_id()->str().empty()) {
            discountCurveId = spec->discount_curve_id()->str();
        }
        if (!discountCurveId.empty() && reg.rates.curves.find(discountCurveId) == reg.rates.curves.end()) {
            QUANTRA_NOT_FOUND("discount_curve_id not found for curve id: " + id + ": " + discountCurveId);
        }

        const auto* idxSpec = findInflationIndexSpec(indices, indexId);
        if (!idxSpec) {
            QUANTRA_NOT_FOUND("Inflation index spec not found for id: " + indexId + " (curve id: " + id + ")");
        }

        QuantLib::Date ref = DateToQL(spec->reference_date()->str());
        QuantLib::Calendar cal = CalendarToQL(spec->calendar());
        QuantLib::BusinessDayConvention bdc = ConventionToQL(spec->business_day_convention());
        QuantLib::DayCounter dc = DayCounterToQL(spec->day_counter());
        const auto kind = spec->kind();
        if (idxSpec->kind() != kind) {
            QUANTRA_INVALID_ARGUMENT("Inflation kind mismatch for curve id '" + id +
                          "': curve.kind and index.kind must match");
        }
        const bool allowExtrap = spec->allow_extrapolation();

        QuantLib::Period obsLag = toQlPeriodReq(idxSpec->observation_lag(), "InflationIndexSpec.observation_lag");
        QuantLib::Period availLag = toQlPeriodReq(idxSpec->availability_lag(), "InflationIndexSpec.availability_lag");
        QuantLib::Frequency freq = FrequencyToQL(idxSpec->frequency());
        const bool idxInterpolated = idxSpec->interpolated();
        std::vector<QuantLib::Date> pillarDates;

        std::unordered_map<std::string, InflationIndexPtr> localIndexCache;
        InflationIndexPtr localIndex = buildInflationIndex(
            idxSpec,
            specMap,
            localIndexCache,
            {},
            {});

        if (kind == enums::InflationCurveKind_ZeroInflation) {
            auto zeroIndex = QuantLib::ext::dynamic_pointer_cast<QuantLib::ZeroInflationIndex>(localIndex);
            if (!zeroIndex) {
                QUANTRA_INVALID_ARGUMENT("Inflation index must be ZeroInflation for curve id: " + id);
            }

            std::vector<QuantLib::ext::shared_ptr<QuantLib::BootstrapHelper<QuantLib::ZeroInflationTermStructure>>> helpers;
            helpers.reserve(spec->points()->size());
            for (flatbuffers::uoffset_t i = 0; i < spec->points()->size(); ++i) {
                const auto* point = spec->points()->Get(i);
                if (!point || point->point_type() != InflationPoint_ZeroCouponInflationSwapHelper) {
                    QUANTRA_INVALID_ARGUMENT("Zero inflation curves require only ZeroCouponInflationSwapHelper points for curve id: " + id);
                }
                const auto* helper = point->point_as_ZeroCouponInflationSwapHelper();
                if (!helper) {
                    QUANTRA_INVALID_ARGUMENT(
                        "Inflation point union type is set but its value is missing for curve id: " + id);
                }
                const std::string label = "ZeroCouponInflationSwapHelper";
                const double quoteValue = resolveQuoteOrInline(helper, quotes, id, label);
                if (!std::isfinite(quoteValue)) {
                    QUANTRA_INVALID_ARGUMENT(label + ".quote_value must be finite for curve id: " + id);
                }
                auto helperCalendar = CalendarToQL(helper->calendar());
                auto helperBdc = ConventionToQL(helper->payment_convention());
                auto helperDc = DayCounterToQL(helper->day_counter());
                auto helperObsLag = toQlPeriodReq(helper->swap_observation_lag(), label + ".swap_observation_lag");
                auto helperInterpolation = cpiInterpolationToQL(helper->observation_interpolation());
                auto dates = resolveHelperDates(helper, ref, helperCalendar, helperBdc, id, label);
                auto quote = QuantLib::Handle<QuantLib::Quote>(
                    QuantLib::ext::make_shared<QuantLib::SimpleQuote>(quoteValue));

                if (dates.useExplicitStartEnd) {
                    helpers.push_back(QuantLib::ext::make_shared<QuantLib::ZeroCouponInflationSwapHelper>(
                        quote,
                        helperObsLag,
                        dates.startDate,
                        dates.endDate,
                        helperCalendar,
                        helperBdc,
                        helperDc,
                        zeroIndex,
                        helperInterpolation));
                } else {
                    helpers.push_back(QuantLib::ext::make_shared<QuantLib::ZeroCouponInflationSwapHelper>(
                        quote,
                        helperObsLag,
                        dates.endDate,
                        helperCalendar,
                        helperBdc,
                        helperDc,
                        zeroIndex,
                        helperInterpolation));
                }
            }

            const QuantLib::Date baseDate = QuantLib::inflationPeriod(ref - availLag, freq).first;
            if (!hasFixingForDate(idxSpec->fixings(), baseDate)) {
                std::ostringstream baseDateMsg;
                baseDateMsg << DateToIso(baseDate);
                QUANTRA_INVALID_ARGUMENT("Zero inflation base fixing unavailable for curve id '" + id +
                              "': provide InflationIndexSpec.fixings for " + baseDateMsg.str());
            }
            try {
                static_cast<void>(zeroIndex->fixing(baseDate));
            } catch (const std::exception& e) {
                QUANTRA_INVALID_ARGUMENT("Zero inflation base fixing unavailable for curve id '" + id + "': " + e.what());
            }

            auto ts = QuantLib::ext::make_shared<QuantLib::PiecewiseZeroInflationCurve<QuantLib::Linear>>(
                ref,
                baseDate,
                freq,
                dc,
                helpers,
                QuantLib::ext::shared_ptr<QuantLib::Seasonality>(),
                spec->bootstrap_accuracy(),
                QuantLib::Linear());
            ts->recalculate();
            if (allowExtrap) {
                ts->enableExtrapolation();
            } else {
                ts->disableExtrapolation();
            }

            auto relink = std::make_shared<QuantLib::RelinkableHandle<QuantLib::ZeroInflationTermStructure>>();
            relink->linkTo(ts);
            reg.inflation.zeroInflationCurves[id] = relink;
            pillarDates = collectPillarDates(helpers);
        } else if (kind == enums::InflationCurveKind_YoYInflation) {
            auto yoyIndex = QuantLib::ext::dynamic_pointer_cast<QuantLib::YoYInflationIndex>(localIndex);
            if (!yoyIndex) {
                QUANTRA_INVALID_ARGUMENT("Inflation index must be YoYInflation for curve id: " + id);
            }

            std::vector<QuantLib::ext::shared_ptr<QuantLib::BootstrapHelper<QuantLib::YoYInflationTermStructure>>> helpers;
            helpers.reserve(spec->points()->size());
            for (flatbuffers::uoffset_t i = 0; i < spec->points()->size(); ++i) {
                const auto* point = spec->points()->Get(i);
                if (!point || point->point_type() != InflationPoint_YearOnYearInflationSwapHelper) {
                    QUANTRA_INVALID_ARGUMENT("YoY inflation curves require only YearOnYearInflationSwapHelper points for curve id: " + id);
                }
                const auto* helper = point->point_as_YearOnYearInflationSwapHelper();
                if (!helper) {
                    QUANTRA_INVALID_ARGUMENT(
                        "Inflation point union type is set but its value is missing for curve id: " + id);
                }
                const std::string label = "YearOnYearInflationSwapHelper";
                const double quoteValue = resolveQuoteOrInline(helper, quotes, id, label);
                if (!std::isfinite(quoteValue)) {
                    QUANTRA_INVALID_ARGUMENT(label + ".quote_value must be finite for curve id: " + id);
                }
                auto helperCalendar = CalendarToQL(helper->calendar());
                auto helperBdc = ConventionToQL(helper->payment_convention());
                auto helperDc = DayCounterToQL(helper->day_counter());
                auto helperObsLag = toQlPeriodReq(helper->swap_observation_lag(), label + ".swap_observation_lag");
                auto helperInterpolation = cpiInterpolationToQL(helper->observation_interpolation());
                auto dates = resolveHelperDates(helper, ref, helperCalendar, helperBdc, id, label);
                auto quote = QuantLib::Handle<QuantLib::Quote>(
                    QuantLib::ext::make_shared<QuantLib::SimpleQuote>(quoteValue));
                auto nominalCurve = lookupNominalCurveHandle<QuantLib::YieldTermStructure>(
                    id, helper->nominal_curve_id()->str(), reg, label);

                if (dates.useExplicitStartEnd) {
                    helpers.push_back(QuantLib::ext::make_shared<QuantLib::YearOnYearInflationSwapHelper>(
                        quote,
                        helperObsLag,
                        dates.startDate,
                        dates.endDate,
                        helperCalendar,
                        helperBdc,
                        helperDc,
                        yoyIndex,
                        helperInterpolation,
                        nominalCurve));
                } else {
                    helpers.push_back(QuantLib::ext::make_shared<QuantLib::YearOnYearInflationSwapHelper>(
                        quote,
                        helperObsLag,
                        dates.endDate,
                        helperCalendar,
                        helperBdc,
                        helperDc,
                        yoyIndex,
                        helperInterpolation,
                        nominalCurve));
                }
            }

            const QuantLib::Date baseDate = QuantLib::inflationPeriod(ref - availLag, freq).first;
            const QuantLib::Rate baseYoYRate = yoyIndex->fixing(baseDate);
            auto ts = QuantLib::ext::make_shared<QuantLib::PiecewiseYoYInflationCurve<QuantLib::Linear>>(
                ref,
                baseDate,
                baseYoYRate,
                freq,
                dc,
                helpers,
                QuantLib::ext::shared_ptr<QuantLib::Seasonality>(),
                spec->bootstrap_accuracy(),
                QuantLib::Linear());
            ts->recalculate();
            if (allowExtrap) {
                ts->enableExtrapolation();
            } else {
                ts->disableExtrapolation();
            }

            auto relink = std::make_shared<QuantLib::RelinkableHandle<QuantLib::YoYInflationTermStructure>>();
            relink->linkTo(ts);
            reg.inflation.yoyInflationCurves[id] = relink;
            pillarDates = collectPillarDates(helpers);
        } else {
            QUANTRA_INVALID_ARGUMENT("Unknown inflation curve kind for curve id: " + id);
        }

        InflationCurveEntry entry;
        entry.kind = kind;
        entry.id = id;
        entry.indexId = indexId;
        entry.referenceDate = ref;
        entry.calendar = cal;
        entry.businessDayConvention = bdc;
        entry.dayCounter = dc;
        entry.observationLag = obsLag;
        entry.frequency = freq;
        entry.indexInterpolated = idxInterpolated;
        entry.allowExtrapolation = allowExtrap;
        entry.pillarDates = pillarDates;
        out.emplace(id, entry);
    }

    return out;
}

void buildInflationIndices(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::InflationIndexSpec>>* indices,
    const std::map<std::string, InflationCurveEntry>& builtCurves,
    PricingRegistry& reg) {
    if (!indices) return;

    const auto specMap = buildInflationIndexSpecMap(indices);
    std::unordered_map<std::string, QuantLib::Handle<QuantLib::ZeroInflationTermStructure>> zeroCurveHandles;
    std::unordered_map<std::string, QuantLib::Handle<QuantLib::YoYInflationTermStructure>> yoyCurveHandles;
    for (const auto& [curveId, meta] : builtCurves) {
        if (meta.kind == enums::InflationCurveKind_ZeroInflation) {
            auto it = reg.inflation.zeroInflationCurves.find(curveId);
            if (it != reg.inflation.zeroInflationCurves.end() && it->second && !it->second->empty()) {
                zeroCurveHandles.emplace(meta.indexId, QuantLib::Handle<QuantLib::ZeroInflationTermStructure>(it->second->currentLink()));
            }
        } else {
            auto it = reg.inflation.yoyInflationCurves.find(curveId);
            if (it != reg.inflation.yoyInflationCurves.end() && it->second && !it->second->empty()) {
                yoyCurveHandles.emplace(meta.indexId, QuantLib::Handle<QuantLib::YoYInflationTermStructure>(it->second->currentLink()));
            }
        }
    }

    std::unordered_map<std::string, InflationIndexPtr> cache;
    for (auto it = indices->begin(); it != indices->end(); ++it) {
        const auto* spec = *it;
        auto index = buildInflationIndex(spec, specMap, cache, zeroCurveHandles, yoyCurveHandles);
        reg.inflation.inflationIndices[spec->id()->str()] = index;
    }
}

} // namespace quantra

