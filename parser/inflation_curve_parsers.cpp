#include "inflation_curve_parsers.h"

#include <algorithm>
#include <unordered_map>

#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/inflation/interpolatedzeroinflationcurve.hpp>
#include <ql/termstructures/inflation/interpolatedyoyinflationcurve.hpp>
#include <ql/indexes/inflationindex.hpp>
#include <ql/currencies/all.hpp>
#include <ql/currencies/europe.hpp>
#include <ql/currencies/america.hpp>
#include <ql/currencies/asia.hpp>
#include <ql/currencies/oceania.hpp>
#include <ql/time/period.hpp>

#include "common.h"
#include "enums.h"
#include "error.h"
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

QuantLib::Period toQlPeriodReq(const quantra::Period* p, const std::string& label) {
    if (!p) {
        QUANTRA_ERROR(label + " is required");
    }
    return QuantLib::Period(p->n(), TimeUnitToQL(p->unit()));
}

double resolveQuoteOrInline(
    const quantra::InflationSwapPillar* p,
    const QuoteRegistry* quotes,
    const std::string& curveId) {
    if (!p) {
        QUANTRA_ERROR("InflationSwapPillar missing for curve id: " + curveId);
    }
    const bool hasQuoteId = p->quote_id() && !p->quote_id()->str().empty();
    if (hasQuoteId) {
        if (!quotes) {
            QUANTRA_ERROR("quote_id requires QuoteRegistry for curve id: " + curveId);
        }
        return quotes->getValue(p->quote_id()->str(), quantra::QuoteType_Curve);
    }
    return p->quote_value();
}

void validateInterpolator(enums::Interpolator i, const std::string& curveId) {
    if (i != enums::Interpolator_Linear) {
        QUANTRA_ERROR("Inflation curve only supports Linear interpolator for curve id: " + curveId);
    }
}

QuantLib::Region regionFromCurrency(const std::string& ccy) {
    if (ccy == "EUR") return QuantLib::EURegion();
    if (ccy == "USD") return QuantLib::USRegion();
    if (ccy == "GBP") return QuantLib::UKRegion();
    if (ccy == "AUD") return QuantLib::AustraliaRegion();
    if (ccy == "ZAR") return QuantLib::ZARegion();
    return QuantLib::CustomRegion("N/A", "N/A");
}

} // namespace

std::map<std::string, InflationCurveEntry> buildInflationCurves(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::InflationCurveSpec>>* curves,
    const flatbuffers::Vector<flatbuffers::Offset<quantra::InflationIndexSpec>>* indices,
    const QuoteRegistry* quotes,
    PricingRegistry& reg) {
    std::map<std::string, InflationCurveEntry> out;
    if (!curves) return out;

    for (auto it = curves->begin(); it != curves->end(); ++it) {
        const auto* spec = *it;
        if (!spec || !spec->base() || !spec->base()->id()) {
            QUANTRA_ERROR("InflationCurveSpec.base.id is required");
        }
        const auto* base = spec->base();
        const std::string id = base->id()->str();
        if (!base->reference_date()) {
            QUANTRA_ERROR("InflationCurveBaseSpec.reference_date is required for curve id: " + id);
        }
        if (!base->index_id() || base->index_id()->str().empty()) {
            QUANTRA_ERROR("InflationCurveBaseSpec.index_id is required for curve id: " + id);
        }
        const std::string indexId = base->index_id()->str();
        std::string discountCurveId;
        if (base->discount_curve_id() && !base->discount_curve_id()->str().empty()) {
            discountCurveId = base->discount_curve_id()->str();
        }
        if (!discountCurveId.empty() && reg.rates.curves.find(discountCurveId) == reg.rates.curves.end()) {
            QUANTRA_ERROR("discount_curve_id not found for curve id: " + id + ": " + discountCurveId);
        }

        const auto* idxSpec = findInflationIndexSpec(indices, indexId);
        if (!idxSpec) {
            QUANTRA_ERROR("Inflation index spec not found for id: " + indexId + " (curve id: " + id + ")");
        }

        QuantLib::Date ref = DateToQL(base->reference_date()->str());
        QuantLib::Calendar cal = CalendarToQL(base->calendar());
        QuantLib::BusinessDayConvention bdc = ConventionToQL(base->business_day_convention());
        QuantLib::DayCounter dc = DayCounterToQL(base->day_counter());
        const auto kind = base->kind();
        if (idxSpec->kind() != kind) {
            QUANTRA_ERROR("Inflation kind mismatch for curve id '" + id +
                          "': base.kind and index.kind must match");
        }
        const bool allowExtrap = base->allow_extrapolation();

        QuantLib::Period obsLag = toQlPeriodReq(idxSpec->observation_lag(), "InflationIndexSpec.observation_lag");
        QuantLib::Period availLag = toQlPeriodReq(idxSpec->availability_lag(), "InflationIndexSpec.availability_lag");
        (void)availLag; // used when building indices
        QuantLib::Frequency freq = FrequencyToQL(idxSpec->frequency());
        const bool idxInterpolated = idxSpec->interpolated();

        std::vector<QuantLib::Date> pillarDates;
        std::vector<QuantLib::Rate> pillarRates;

        auto addPillars = [&](const flatbuffers::Vector<flatbuffers::Offset<quantra::InflationSwapPillar>>* pillars,
                              enums::Interpolator interp) {
            if (!pillars || pillars->size() == 0) {
                QUANTRA_ERROR("Inflation curve pillars are required for curve id: " + id);
            }
            validateInterpolator(interp, id);
            pillarDates.reserve(pillars->size());
            pillarRates.reserve(pillars->size());
            for (flatbuffers::uoffset_t i = 0; i < pillars->size(); ++i) {
                const auto* p = pillars->Get(i);
                if (!p || !p->maturity()) {
                    QUANTRA_ERROR("InflationSwapPillar.maturity is required for curve id: " + id);
                }
                QuantLib::Period mat(p->maturity()->n(), TimeUnitToQL(p->maturity()->unit()));
                if (mat.length() <= 0) {
                    QUANTRA_ERROR("InflationSwapPillar.maturity must be > 0 for curve id: " + id);
                }
                QuantLib::Date d = cal.advance(ref, mat, bdc);
                if (d <= ref) {
                    QUANTRA_ERROR("Inflation curve pillar dates must be after reference_date for curve id: " + id);
                }
                if (!pillarDates.empty() && d <= pillarDates.back()) {
                    QUANTRA_ERROR("Inflation curve pillar maturities must be strictly increasing for curve id: " + id);
                }
                double q = resolveQuoteOrInline(p, quotes, id);
                if (!std::isfinite(q)) {
                    QUANTRA_ERROR("Inflation curve pillar quote must be finite for curve id: " + id);
                }
                pillarDates.push_back(d);
                pillarRates.push_back(q);
            }
        };

        if (spec->payload_type() == InflationCurvePayload_ZeroInflationCurveFromZcSwapsSpec) {
            if (kind != enums::InflationCurveKind_ZeroInflation) {
                QUANTRA_ERROR("InflationCurvePayload ZeroInflationCurveFromZcSwapsSpec requires kind=ZeroInflation for curve id: " + id);
            }
            const auto* payload = spec->payload_as_ZeroInflationCurveFromZcSwapsSpec();
            if (!payload) {
                QUANTRA_ERROR("ZeroInflationCurve payload missing for curve id: " + id);
            }
            addPillars(payload->pillars(), payload->interpolator());

            // QuantLib expects the first node to be the base date; ensure at least two nodes.
            std::vector<QuantLib::Date> dates = pillarDates;
            std::vector<QuantLib::Rate> rates = pillarRates;
            if (dates.empty()) {
                QUANTRA_ERROR("Inflation curve requires at least one pillar for curve id: " + id);
            }
            dates.insert(dates.begin(), ref);
            rates.insert(rates.begin(), rates.front());

            auto ts = QuantLib::ext::make_shared<QuantLib::InterpolatedZeroInflationCurve<QuantLib::Linear>>(
                ref, dates, rates, freq, dc, nullptr, QuantLib::Linear());
            QuantLib::Handle<QuantLib::ZeroInflationTermStructure> h(ts);
            if (allowExtrap) h->enableExtrapolation();
            else h->disableExtrapolation();

            auto relink = std::make_shared<QuantLib::RelinkableHandle<QuantLib::ZeroInflationTermStructure>>();
            relink->linkTo(h.currentLink());
            reg.inflation.zeroInflationCurves[id] = relink;
        } else if (spec->payload_type() == InflationCurvePayload_YoYInflationCurveFromYoYSwapsSpec) {
            if (kind != enums::InflationCurveKind_YoYInflation) {
                QUANTRA_ERROR("InflationCurvePayload YoYInflationCurveFromYoYSwapsSpec requires kind=YoYInflation for curve id: " + id);
            }
            const auto* payload = spec->payload_as_YoYInflationCurveFromYoYSwapsSpec();
            if (!payload) {
                QUANTRA_ERROR("YoYInflationCurve payload missing for curve id: " + id);
            }
            addPillars(payload->pillars(), payload->interpolator());

            std::vector<QuantLib::Date> dates = pillarDates;
            std::vector<QuantLib::Rate> rates = pillarRates;
            if (dates.empty()) {
                QUANTRA_ERROR("Inflation curve requires at least one pillar for curve id: " + id);
            }
            dates.insert(dates.begin(), ref);
            rates.insert(rates.begin(), rates.front());

            auto ts = QuantLib::ext::make_shared<QuantLib::InterpolatedYoYInflationCurve<QuantLib::Linear>>(
                ref, dates, rates, freq, dc, nullptr, QuantLib::Linear());
            QuantLib::Handle<QuantLib::YoYInflationTermStructure> h(ts);
            if (allowExtrap) h->enableExtrapolation();
            else h->disableExtrapolation();

            auto relink = std::make_shared<QuantLib::RelinkableHandle<QuantLib::YoYInflationTermStructure>>();
            relink->linkTo(h.currentLink());
            reg.inflation.yoyInflationCurves[id] = relink;
        } else {
            QUANTRA_ERROR("Unknown InflationCurvePayload type for curve id: " + id);
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

    // index_id -> candidate curves (allow multiple curves per index_id)
    std::unordered_map<std::string, std::vector<std::string>> indexToCurves;
    for (const auto& kv : builtCurves) {
        indexToCurves[kv.second.indexId].push_back(kv.second.id);
    }

    for (auto it = indices->begin(); it != indices->end(); ++it) {
        const auto* s = *it;
        if (!s || !s->id()) {
            QUANTRA_ERROR("InflationIndexSpec.id is required");
        }
        const std::string id = s->id()->str();
        const std::string family = (s->family_name() && !s->family_name()->str().empty())
            ? s->family_name()->str()
            : id;

        // Link index to its curve if present; if no curve provided, keep empty.
        QuantLib::Currency ccy = CurrencyFromString((s->currency() && !s->currency()->str().empty()) ? s->currency()->str() : "EUR");
        QuantLib::Calendar cal = CalendarToQL(s->calendar());
        QuantLib::DayCounter dc = DayCounterToQL(s->day_counter());
        QuantLib::Frequency freq = FrequencyToQL(s->frequency());
        QuantLib::Period availLag = toQlPeriodReq(s->availability_lag(), "InflationIndexSpec.availability_lag");
        const bool revised = s->revised();
        const bool interpolated = s->interpolated();

        const std::string ccyStr = (s->currency() && !s->currency()->str().empty()) ? s->currency()->str() : "EUR";
        QuantLib::Region region = regionFromCurrency(ccyStr);

        auto curveIt = indexToCurves.find(id);
        if (curveIt == indexToCurves.end()) {
            // Index defined but no curve supplied: still allow creation with empty term structure.
            if (s->kind() == enums::InflationCurveKind_ZeroInflation) {
                auto idx = QuantLib::ext::make_shared<QuantLib::ZeroInflationIndex>(
                    family,
                    region,
                    revised,
                    freq,
                    availLag,
                    ccy);
                reg.inflation.inflationIndices[id] = idx;
            } else {
                auto idx = QuantLib::ext::make_shared<QuantLib::YoYInflationIndex>(
                    family,
                    region,
                    revised,
                    freq,
                    availLag,
                    ccy);
                reg.inflation.inflationIndices[id] = idx;
            }
            continue;
        }

        std::string curveId;
        for (const auto& candidateId : curveIt->second) {
            const auto& candidate = builtCurves.at(candidateId);
            if (candidate.kind == s->kind()) {
                curveId = candidateId;
                break;
            }
        }
        if (curveId.empty()) {
            // No matching kind for this index_id: keep index unlinked rather than hard-failing.
            if (s->kind() == enums::InflationCurveKind_ZeroInflation) {
                auto idx = QuantLib::ext::make_shared<QuantLib::ZeroInflationIndex>(
                    family, region, revised, freq, availLag, ccy);
                reg.inflation.inflationIndices[id] = idx;
            } else {
                auto idx = QuantLib::ext::make_shared<QuantLib::YoYInflationIndex>(
                    family, region, revised, freq, availLag, ccy);
                reg.inflation.inflationIndices[id] = idx;
            }
            continue;
        }

        const auto& curveMeta = builtCurves.at(curveId);

        if (curveMeta.kind == enums::InflationCurveKind_ZeroInflation) {
            auto tsIt = reg.inflation.zeroInflationCurves.find(curveId);
            if (tsIt == reg.inflation.zeroInflationCurves.end() || !tsIt->second || tsIt->second->empty()) {
                QUANTRA_ERROR("Zero inflation curve handle missing for curve id: " + curveId);
            }
            QuantLib::Handle<QuantLib::ZeroInflationTermStructure> h(tsIt->second->currentLink());
            auto idx = QuantLib::ext::make_shared<QuantLib::ZeroInflationIndex>(
                family,
                region,
                revised,
                freq,
                availLag,
                ccy,
                h);
            reg.inflation.inflationIndices[id] = idx;
        } else {
            auto tsIt = reg.inflation.yoyInflationCurves.find(curveId);
            if (tsIt == reg.inflation.yoyInflationCurves.end() || !tsIt->second || tsIt->second->empty()) {
                QUANTRA_ERROR("YoY inflation curve handle missing for curve id: " + curveId);
            }
            QuantLib::Handle<QuantLib::YoYInflationTermStructure> h(tsIt->second->currentLink());
            auto idx = QuantLib::ext::make_shared<QuantLib::YoYInflationIndex>(
                family,
                region,
                revised,
                freq,
                availLag,
                ccy,
                h);
            reg.inflation.inflationIndices[id] = idx;
        }
    }
}

} // namespace quantra

