#include "term_structure_point_parser.h"

#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/instruments/makeois.hpp>

using namespace QuantLib;

namespace quantra {

// =============================================================================
// Quote resolution
// =============================================================================

Handle<Quote> TermStructurePointParser::resolveQuote(
    double inlineValue,
    const flatbuffers::String* quoteId,
    const QuoteRegistry* quotes
) const {
    if (quoteId && quotes) {
        const std::string id = quoteId->str();
        if (!id.empty()) {
            return quotes->getHandle(id);
        }
    }
    auto sq = std::make_shared<SimpleQuote>(inlineValue);
    return Handle<Quote>(sq);
}

// =============================================================================
// Exogenous curve resolution
// =============================================================================

Handle<YieldTermStructure> TermStructurePointParser::resolveCurve(
    const quantra::CurveRef* ref,
    const CurveRegistry* curves
) const {
    if (!ref || !ref->id()) return Handle<YieldTermStructure>();
    if (!curves) {
        QUANTRA_ERROR("Curve dependency '" + ref->id()->str() +
                      "' specified but CurveRegistry is null");
    }
    return curves->get(ref->id()->str());
}

// =============================================================================
// Main parse dispatcher
// =============================================================================

std::shared_ptr<RateHelper> TermStructurePointParser::parse(
    uint8_t point_type,
    const void* data,
    const QuoteRegistry* quotes,
    const CurveRegistry* curves,
    const IndexRegistry* indices
) const {

    // ------------------------------------------------------------------
    // Deposit
    // ------------------------------------------------------------------
    if (point_type == quantra::Point_DepositHelper) {
        auto point = static_cast<const quantra::DepositHelper*>(data);
        auto q = resolveQuote(point->rate(), point->quote_id(), quotes);

        return std::make_shared<DepositRateHelper>(
            q,
            point->tenor_number() * TimeUnitToQL(point->tenor_time_unit()),
            point->fixing_days(),
            CalendarToQL(point->calendar()),
            ConventionToQL(point->business_day_convention()),
            true,
            DayCounterToQL(point->day_counter()));
    }

    // ------------------------------------------------------------------
    // FRA
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_FRAHelper) {
        auto point = static_cast<const quantra::FRAHelper*>(data);
        auto q = resolveQuote(point->rate(), point->quote_id(), quotes);

        return std::make_shared<FraRateHelper>(
            q,
            point->months_to_start(),
            point->months_to_end(),
            point->fixing_days(),
            CalendarToQL(point->calendar()),
            ConventionToQL(point->business_day_convention()),
            true,
            DayCounterToQL(point->day_counter()));
    }

    // ------------------------------------------------------------------
    // Futures
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_FutureHelper) {
        auto point = static_cast<const quantra::FutureHelper*>(data);

        double rateValue = point->rate();
        if (point->futures_price() != 0.0) {
            rateValue = 1.0 - (point->futures_price() / 100.0);
        }
        rateValue += point->convexity_adjustment();

        auto q = resolveQuote(rateValue, point->quote_id(), quotes);

        return std::make_shared<FuturesRateHelper>(
            q,
            DateToQL(point->future_start_date()->str()),
            point->future_months(),
            CalendarToQL(point->calendar()),
            ConventionToQL(point->business_day_convention()),
            true,
            DayCounterToQL(point->day_counter()));
    }

    // ------------------------------------------------------------------
    // Vanilla IBOR Swap (index resolved from IndexRegistry)
    //
    // Forwarding curve: ALWAYS the curve being bootstrapped.
    //
    // QuantLib's SwapRateHelper::initializeDates() internally does:
    //   iborIndex_->clone(termStructureHandle_)
    // where termStructureHandle_ is linked to the curve being bootstrapped
    // via setTermStructure(). This means ANY forwarding handle we attach
    // to the index gets overwritten — the helper always projects off the
    // curve being built. This is by design in QuantLib.
    //
    // Therefore:
    //   - We pass a "naked" index (no forwarding handle) from IndexRegistry
    //   - The helper's bootstrap machinery attaches the correct handle
    //   - deps.discount_curve provides the exogenous discount curve for
    //     multi-curve setups (e.g., Euribor curve discounted with OIS)
    //
    // NOTE: deps.projection_curve is NOT supported for SwapHelper because
    // QuantLib's SwapRateHelper always overrides the index forwarding
    // handle. Supporting exogenous projection would require custom
    // RateHelper subclasses that override setTermStructure() behavior.
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_SwapHelper) {
        auto point = static_cast<const quantra::SwapHelper*>(data);
        auto q = resolveQuote(point->rate(), point->quote_id(), quotes);

        if (!indices) {
            QUANTRA_ERROR("IndexRegistry is required for SwapHelper");
        }
        if (!point->float_index() || !point->float_index()->id()) {
            QUANTRA_ERROR("SwapHelper.float_index.id is required");
        }

        std::string indexId = point->float_index()->id()->str();

        // Hard error: projection_curve is not supported for SwapHelper
        if (point->deps() && point->deps()->projection_curve() &&
            point->deps()->projection_curve()->id()) {
            QUANTRA_ERROR(
                "projection_curve is not supported for SwapHelper. "
                "QuantLib's SwapRateHelper::setTermStructure() overrides the index "
                "forwarding handle with the curve being bootstrapped. "
                "Use deps.discount_curve for multi-curve discounting (e.g., Euribor "
                "curve discounted with OIS). For true multi-curve projection "
                "relationships, use basis/XCCY/FX helpers (future).");
        }

        // Naked index — QuantLib bootstrap clones it with curve being built
        auto ibor = indices->getIbor(indexId);

        auto spread = Handle<Quote>(std::make_shared<SimpleQuote>(point->spread()));

        // Resolve exogenous discount curve
        Handle<YieldTermStructure> discount;
        if (point->deps() && point->deps()->discount_curve()) {
            discount = resolveCurve(point->deps()->discount_curve(), curves);
        }

        return std::make_shared<SwapRateHelper>(
            q,
            point->tenor_number() * TimeUnitToQL(point->tenor_time_unit()),
            CalendarToQL(point->calendar()),
            FrequencyToQL(point->sw_fixed_leg_frequency()),
            ConventionToQL(point->sw_fixed_leg_convention()),
            DayCounterToQL(point->sw_fixed_leg_day_counter()),
            ibor,
            spread,
            point->fwd_start_days() * Days,
            discount
        );
    }

    // ------------------------------------------------------------------
    // Bond helper
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_BondHelper) {
        auto point = static_cast<const quantra::BondHelper*>(data);
        ScheduleParser schedule_parser;

        double px = point->price();
        if (px == 0.0) px = point->rate();

        auto q = resolveQuote(px, point->quote_id(), quotes);

        return std::make_shared<FixedRateBondHelper>(
            q,
            point->settlement_days(),
            point->face_amount(),
            *schedule_parser.parse(point->schedule()),
            std::vector<Rate>(1, point->coupon_rate()),
            DayCounterToQL(point->day_counter()),
            ConventionToQL(point->business_day_convention()),
            point->redemption(),
            DateToQL(point->issue_date()->str()));
    }

    // ------------------------------------------------------------------
    // OIS helper (index resolved from IndexRegistry)
    //
    // Same as SwapHelper: QuantLib's OISRateHelper::initializeDates()
    // builds the OIS swap using MakeOIS with the stored overnightIndex_,
    // and setTermStructure() links termStructureHandle_ to the curve
    // being bootstrapped. The overnight index forwarding is always tied
    // to the curve being built.
    //
    // deps.discount_curve provides exogenous discounting for dual-curve
    // OIS (e.g., OIS discounted off a funding curve).
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_OISHelper) {
        auto point = static_cast<const quantra::OISHelper*>(data);
        auto q = resolveQuote(point->rate(), point->quote_id(), quotes);

        if (!indices) {
            QUANTRA_ERROR("IndexRegistry is required for OISHelper");
        }
        if (!point->overnight_index() || !point->overnight_index()->id()) {
            QUANTRA_ERROR("OISHelper.overnight_index.id is required");
        }

        std::string indexId = point->overnight_index()->id()->str();

        // Hard error: projection_curve is not supported for OISHelper
        if (point->deps() && point->deps()->projection_curve() &&
            point->deps()->projection_curve()->id()) {
            QUANTRA_ERROR(
                "projection_curve is not supported for OISHelper. "
                "QuantLib's OISRateHelper::setTermStructure() overrides the overnight "
                "index forwarding handle with the curve being bootstrapped. "
                "Use deps.discount_curve for dual-curve OIS discounting. "
                "For true multi-curve projection, use basis/XCCY helpers (future).");
        }

        auto on = indices->getOvernight(indexId);

        // Resolve exogenous discount curve (for dual-curve OIS)
        Handle<YieldTermStructure> discount;
        if (point->deps() && point->deps()->discount_curve()) {
            discount = resolveCurve(point->deps()->discount_curve(), curves);
        }

        return std::make_shared<OISRateHelper>(
            point->settlement_days(),
            point->tenor_number() * TimeUnitToQL(point->tenor_time_unit()),
            q,
            on,
            discount
        );
    }

    // ------------------------------------------------------------------
    // Dated OIS helper (index resolved from IndexRegistry)
    // Same projection limitation as OISHelper above.
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_DatedOISHelper) {
        auto point = static_cast<const quantra::DatedOISHelper*>(data);
        auto q = resolveQuote(point->rate(), point->quote_id(), quotes);

        if (!indices) {
            QUANTRA_ERROR("IndexRegistry is required for DatedOISHelper");
        }
        if (!point->overnight_index() || !point->overnight_index()->id()) {
            QUANTRA_ERROR("DatedOISHelper.overnight_index.id is required");
        }

        std::string indexId = point->overnight_index()->id()->str();

        // Hard error: projection_curve is not supported for DatedOISHelper
        if (point->deps() && point->deps()->projection_curve() &&
            point->deps()->projection_curve()->id()) {
            QUANTRA_ERROR(
                "projection_curve is not supported for DatedOISHelper. "
                "QuantLib's OISRateHelper overrides the overnight index forwarding "
                "handle with the curve being bootstrapped. "
                "Use deps.discount_curve for dual-curve OIS discounting.");
        }

        auto on = indices->getOvernight(indexId);

        Date start = DateToQL(point->start_date()->str());
        Date end   = DateToQL(point->end_date()->str());

        // Resolve exogenous discount curve
        Handle<YieldTermStructure> discount;
        if (point->deps() && point->deps()->discount_curve()) {
            discount = resolveCurve(point->deps()->discount_curve(), curves);
        }

        return std::make_shared<DatedOISRateHelper>(
            start, end, q, on, discount);
    }

    // ------------------------------------------------------------------
    // Stubs for basis / FX / XCCY
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_TenorBasisSwapHelper) {
        QUANTRA_ERROR("TenorBasisSwapHelper: schema-ready but not implemented.");
    }
    else if (point_type == quantra::Point_FxSwapHelper) {
        QUANTRA_ERROR("FxSwapHelper: schema-ready but not implemented.");
    }
    else if (point_type == quantra::Point_CrossCcyBasisHelper) {
        QUANTRA_ERROR("CrossCcyBasisHelper: schema-ready but not implemented.");
    }

    QUANTRA_ERROR("Unknown point type: " + std::to_string(point_type));
    return nullptr;
}

} // namespace quantra
