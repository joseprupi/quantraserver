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
    // Inline fallback: create a standalone SimpleQuote
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
    const IndexFactory* indexFactory
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

        // If futures_price is set, use it; otherwise use rate
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
    // Vanilla IBOR Swap (with optional exogenous discount curve)
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_SwapHelper) {
        auto point = static_cast<const quantra::SwapHelper*>(data);
        auto q = resolveQuote(point->rate(), point->quote_id(), quotes);

        // Build the floating leg index
        std::shared_ptr<IborIndex> ibor;

        // Prefer data-driven IndexSpec if available
        if (point->float_index_type() == quantra::IndexSpec_IborIndexSpec && indexFactory) {
            ibor = indexFactory->makeIborIndexFromSpec(
                point->float_index_as_IborIndexSpec());
        } else if (indexFactory) {
            // Use legacy enum
            ibor = indexFactory->makeIborIndex(point->sw_floating_leg_index());
        } else {
            // Fallback to global IborToQL (no forwarding curve)
            ibor = IborToQL(point->sw_floating_leg_index());
        }

        auto spread = Handle<Quote>(std::make_shared<SimpleQuote>(point->spread()));

        // Resolve exogenous discount curve
        Handle<YieldTermStructure> discount;
        if (point->deps() && point->deps()->discount_curve()) {
            discount = resolveCurve(point->deps()->discount_curve(), curves);
        }

        // Build SwapRateHelper
        // QuantLib >= 1.15 supports passing a discount curve to SwapRateHelper
        // via the 10th constructor parameter.
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
            discount  // exogenous discount (empty handle = self-discounting)
        );
    }

    // ------------------------------------------------------------------
    // Bond helper
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_BondHelper) {
        auto point = static_cast<const quantra::BondHelper*>(data);
        ScheduleParser schedule_parser;

        // Prefer price field; fall back to rate for backward compat
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
    // OIS helper
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_OISHelper) {
        auto point = static_cast<const quantra::OISHelper*>(data);
        auto q = resolveQuote(point->rate(), point->quote_id(), quotes);

        // Build overnight index
        std::shared_ptr<OvernightIndex> on;
        if (point->overnight_index_spec() && indexFactory) {
            on = indexFactory->makeOvernightIndexFromSpec(point->overnight_index_spec());
        } else if (indexFactory) {
            on = indexFactory->makeOvernightIndex(point->overnight_index());
        } else {
            QUANTRA_ERROR("IndexFactory is required for OISHelper");
        }

        return std::make_shared<OISRateHelper>(
            point->settlement_days(),
            point->tenor_number() * TimeUnitToQL(point->tenor_time_unit()),
            q,
            on);
    }

    // ------------------------------------------------------------------
    // Dated OIS helper - using OISRateHelper (DatedOISRateHelper is deprecated)
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_DatedOISHelper) {
        auto point = static_cast<const quantra::DatedOISHelper*>(data);
        auto q = resolveQuote(point->rate(), point->quote_id(), quotes);

        std::shared_ptr<OvernightIndex> on;
        if (point->overnight_index_spec() && indexFactory) {
            on = indexFactory->makeOvernightIndexFromSpec(point->overnight_index_spec());
        } else if (indexFactory) {
            on = indexFactory->makeOvernightIndex(point->overnight_index());
        } else {
            QUANTRA_ERROR("IndexFactory is required for DatedOISHelper");
        }

        Date start = DateToQL(point->start_date()->str());
        Date end   = DateToQL(point->end_date()->str());

        // Use OISRateHelper with explicit start/end dates constructor
        // QuantLib 1.31+ deprecates DatedOISRateHelper in favor of OISRateHelper
        // OISRateHelper has a constructor that takes (startDate, endDate, quote, index)
        return std::make_shared<OISRateHelper>(
            start, end, q, on);
    }

    // ------------------------------------------------------------------
    // Stubs for basis / FX / XCCY (schema-ready, not yet implemented)
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_TenorBasisSwapHelper) {
        QUANTRA_ERROR("TenorBasisSwapHelper: schema-ready but not implemented. "
                      "Add QuantLib TenorBasisSwap helper wiring + deps resolution.");
    }
    else if (point_type == quantra::Point_FxSwapHelper) {
        QUANTRA_ERROR("FxSwapHelper: schema-ready but not implemented. "
                      "Requires FX spot quote + domestic/foreign curves.");
    }
    else if (point_type == quantra::Point_CrossCcyBasisHelper) {
        QUANTRA_ERROR("CrossCcyBasisHelper: schema-ready but not implemented. "
                      "Requires FX spot + 2 curves + basis helper.");
    }

    QUANTRA_ERROR("Unknown point type: " + std::to_string(point_type));
    return nullptr;
}

} // namespace quantra
