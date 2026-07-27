#include "term_structure_point_parser.h"

#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/instruments/makeois.hpp>
#include <ql/settings.hpp>

#include "require_scalar.h"

using namespace QuantLib;

namespace quantra {

// =============================================================================
// Quote resolution
// =============================================================================

Handle<Quote> TermStructurePointParser::resolveQuote(
    double inlineValue,
    const flatbuffers::String* quoteId,
    const QuoteRegistry* quotes,
    quantra::QuoteType expectedType,
    double bump
) const {
    if (quoteId && quotes) {
        const std::string id = quoteId->str();
        if (!id.empty()) {
            if (bump == 0.0) {
                return quotes->getHandle(id, expectedType);
            }
            double v = quotes->getValue(id, expectedType) + bump;
            auto sq = std::make_shared<SimpleQuote>(v);
            return Handle<Quote>(sq);
        }
    }
    auto sq = std::make_shared<SimpleQuote>(inlineValue + bump);
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
    const IndexRegistry* indices,
    double bump
) const {

    // A union whose type byte names a helper but whose value offset is absent
    // survives FlatBuffers verification; the static_casts in each branch below
    // would then dereference a null table and crash. Reject it up front.
    if (point_type != quantra::Point_NONE && data == nullptr) {
        QUANTRA_INVALID_ARGUMENT(
            "Term structure point union type is set but its value is missing");
    }

    // ------------------------------------------------------------------
    // Deposit
    // ------------------------------------------------------------------
    if (point_type == quantra::Point_DepositHelper) {
        auto point = static_cast<const quantra::DepositHelper*>(data);
        if (!point->tenor()) {
            QUANTRA_INVALID_ARGUMENT("DepositHelper.tenor is required");
        }
        const bool has_quote = point->quote_id() && !point->quote_id()->str().empty();
        double rateValue;
        if (point->rate().has_value()) {
            rateValue = point->rate().value();
        } else if (has_quote) {
            rateValue = 0.0; // unused: quote_id supplies the rate below
        } else {
            QUANTRA_INVALID_ARGUMENT("DepositHelper requires one of rate or quote_id");
        }
        auto q = resolveQuote(rateValue, point->quote_id(), quotes, quantra::QuoteType_Curve, bump);

        if (!point->calendar().has_value())
            QUANTRA_INVALID_ARGUMENT("DepositHelper.calendar is required");
        if (!point->business_day_convention().has_value())
            QUANTRA_INVALID_ARGUMENT("DepositHelper.business_day_convention is required");
        if (!point->day_counter().has_value())
            QUANTRA_INVALID_ARGUMENT("DepositHelper.day_counter is required");

        return std::make_shared<DepositRateHelper>(
            q,
            point->tenor()->n() * TimeUnitToQL(point->tenor()->unit()),
            point->fixing_days(),
            CalendarToQL(point->calendar().value()),
            ConventionToQL(point->business_day_convention().value()),
            true,
            DayCounterToQL(point->day_counter().value()));
    }

    // ------------------------------------------------------------------
    // FRA
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_FRAHelper) {
        auto point = static_cast<const quantra::FRAHelper*>(data);
        const bool has_quote = point->quote_id() && !point->quote_id()->str().empty();
        double rateValue;
        if (point->rate().has_value()) {
            rateValue = point->rate().value();
        } else if (has_quote) {
            rateValue = 0.0; // unused: quote_id supplies the rate below
        } else {
            QUANTRA_INVALID_ARGUMENT("FRAHelper requires one of rate or quote_id");
        }
        auto q = resolveQuote(rateValue, point->quote_id(), quotes, quantra::QuoteType_Curve, bump);

        if (!point->calendar().has_value())
            QUANTRA_INVALID_ARGUMENT("FRAHelper.calendar is required");
        if (!point->business_day_convention().has_value())
            QUANTRA_INVALID_ARGUMENT("FRAHelper.business_day_convention is required");
        if (!point->day_counter().has_value())
            QUANTRA_INVALID_ARGUMENT("FRAHelper.day_counter is required");

        return std::make_shared<FraRateHelper>(
            q,
            point->months_to_start(),
            point->months_to_end(),
            point->fixing_days(),
            CalendarToQL(point->calendar().value()),
            ConventionToQL(point->business_day_convention().value()),
            true,
            DayCounterToQL(point->day_counter().value()));
    }

    // ------------------------------------------------------------------
    // Futures
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_FutureHelper) {
        auto point = static_cast<const quantra::FutureHelper*>(data);

        const bool has_quote = point->quote_id() && !point->quote_id()->str().empty();
        // QuantLib's FuturesRateHelper takes the futures PRICE (0-100 IMM
        // scale) as its quote and handles convexity as a dedicated argument.
        double priceValue;
        if (point->futures_price().has_value()) {
            // futures_price takes precedence and is the price itself.
            priceValue = point->futures_price().value();
        } else if (point->rate().has_value()) {
            // Definitional futures price<->rate identity: price = 100*(1-rate).
            priceValue = 100.0 * (1.0 - point->rate().value());
        } else if (has_quote) {
            priceValue = 0.0; // unused: quote_id supplies the price below
        } else {
            QUANTRA_INVALID_ARGUMENT(
                "FutureHelper requires one of futures_price, rate or quote_id");
        }

        auto q = resolveQuote(priceValue, point->quote_id(), quotes, quantra::QuoteType_Curve, bump);

        if (!point->future_start_date()) {
            QUANTRA_INVALID_ARGUMENT("FutureHelper.future_start_date is required");
        }
        if (!point->calendar().has_value())
            QUANTRA_INVALID_ARGUMENT("FutureHelper.calendar is required");
        if (!point->business_day_convention().has_value())
            QUANTRA_INVALID_ARGUMENT("FutureHelper.business_day_convention is required");
        if (!point->day_counter().has_value())
            QUANTRA_INVALID_ARGUMENT("FutureHelper.day_counter is required");

        auto convexity = Handle<Quote>(
            std::make_shared<SimpleQuote>(point->convexity_adjustment()));

        return std::make_shared<FuturesRateHelper>(
            q,
            DateToQL(point->future_start_date()->str()),
            point->future_months(),
            CalendarToQL(point->calendar().value()),
            ConventionToQL(point->business_day_convention().value()),
            true,
            DayCounterToQL(point->day_counter().value()),
            convexity);
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
        if (!point->tenor()) {
            QUANTRA_INVALID_ARGUMENT("SwapHelper.tenor is required");
        }
        const bool has_quote = point->quote_id() && !point->quote_id()->str().empty();
        double rateValue;
        if (point->rate().has_value()) {
            rateValue = point->rate().value();
        } else if (has_quote) {
            rateValue = 0.0; // unused: quote_id supplies the rate below
        } else {
            QUANTRA_INVALID_ARGUMENT("SwapHelper requires one of rate or quote_id");
        }
        auto q = resolveQuote(rateValue, point->quote_id(), quotes, quantra::QuoteType_Curve, bump);

        if (!indices) {
            QUANTRA_ERROR("IndexRegistry is required for SwapHelper");
        }
        if (!point->float_index() || !point->float_index()->id()) {
            QUANTRA_INVALID_ARGUMENT("SwapHelper.float_index.id is required");
        }

        std::string indexId = point->float_index()->id()->str();

        // Hard error: projection_curve is not supported for SwapHelper
        if (point->deps() && point->deps()->projection_curve() &&
            point->deps()->projection_curve()->id()) {
            QUANTRA_INVALID_ARGUMENT(
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

        if (!point->calendar().has_value())
            QUANTRA_INVALID_ARGUMENT("SwapHelper.calendar is required");
        if (!point->sw_fixed_leg_frequency().has_value())
            QUANTRA_INVALID_ARGUMENT("SwapHelper.sw_fixed_leg_frequency is required");
        if (!point->sw_fixed_leg_convention().has_value())
            QUANTRA_INVALID_ARGUMENT("SwapHelper.sw_fixed_leg_convention is required");
        if (!point->sw_fixed_leg_day_counter().has_value())
            QUANTRA_INVALID_ARGUMENT("SwapHelper.sw_fixed_leg_day_counter is required");

        return std::make_shared<SwapRateHelper>(
            q,
            point->tenor()->n() * TimeUnitToQL(point->tenor()->unit()),
            CalendarToQL(point->calendar().value()),
            FrequencyToQL(point->sw_fixed_leg_frequency().value()),
            ConventionToQL(point->sw_fixed_leg_convention().value()),
            DayCounterToQL(point->sw_fixed_leg_day_counter().value()),
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

        const bool has_quote = point->quote_id() && !point->quote_id()->str().empty();
        double px;
        if (point->price().has_value()) {
            px = point->price().value();
        } else if (point->rate().has_value()) {
            px = point->rate().value();
        } else if (has_quote) {
            px = 0.0; // unused: quote_id supplies the value below
        } else {
            QUANTRA_INVALID_ARGUMENT(
                "BondHelper requires one of price, rate or quote_id");
        }

        auto q = resolveQuote(px, point->quote_id(), quotes, quantra::QuoteType_Curve, bump);

        if (!point->schedule()) {
            QUANTRA_INVALID_ARGUMENT("BondHelper.schedule is required");
        }
        if (!point->issue_date()) {
            QUANTRA_INVALID_ARGUMENT("BondHelper.issue_date is required");
        }
        if (!point->day_counter().has_value())
            QUANTRA_INVALID_ARGUMENT("BondHelper.day_counter is required");
        if (!point->business_day_convention().has_value())
            QUANTRA_INVALID_ARGUMENT("BondHelper.business_day_convention is required");

        const double faceAmount = requirePositive(point->face_amount(), "BondHelper.face_amount");
        const double couponRate = requireFinite(point->coupon_rate(), "BondHelper.coupon_rate");
        const double redemption = requirePositive(point->redemption(), "BondHelper.redemption");

        return std::make_shared<FixedRateBondHelper>(
            q,
            point->settlement_days(),
            faceAmount,
            *schedule_parser.parse(point->schedule()),
            std::vector<Rate>(1, couponRate),
            DayCounterToQL(point->day_counter().value()),
            ConventionToQL(point->business_day_convention().value()),
            redemption,
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
        if (!point->tenor()) {
            QUANTRA_INVALID_ARGUMENT("OISHelper.tenor is required");
        }
        const bool has_quote = point->quote_id() && !point->quote_id()->str().empty();
        double rateValue;
        if (point->rate().has_value()) {
            rateValue = point->rate().value();
        } else if (has_quote) {
            rateValue = 0.0; // unused: quote_id supplies the rate below
        } else {
            QUANTRA_INVALID_ARGUMENT("OISHelper requires one of rate or quote_id");
        }
        auto q = resolveQuote(rateValue, point->quote_id(), quotes, quantra::QuoteType_Curve, bump);

        if (!indices) {
            QUANTRA_ERROR("IndexRegistry is required for OISHelper");
        }
        if (!point->overnight_index() || !point->overnight_index()->id()) {
            QUANTRA_INVALID_ARGUMENT("OISHelper.overnight_index.id is required");
        }

        std::string indexId = point->overnight_index()->id()->str();

        // Hard error: projection_curve is not supported for OISHelper
        if (point->deps() && point->deps()->projection_curve() &&
            point->deps()->projection_curve()->id()) {
            QUANTRA_INVALID_ARGUMENT(
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
            point->tenor()->n() * TimeUnitToQL(point->tenor()->unit()),
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
        const bool has_quote = point->quote_id() && !point->quote_id()->str().empty();
        double rateValue;
        if (point->rate().has_value()) {
            rateValue = point->rate().value();
        } else if (has_quote) {
            rateValue = 0.0; // unused: quote_id supplies the rate below
        } else {
            QUANTRA_INVALID_ARGUMENT("DatedOISHelper requires one of rate or quote_id");
        }
        auto q = resolveQuote(rateValue, point->quote_id(), quotes, quantra::QuoteType_Curve, bump);

        if (!indices) {
            QUANTRA_ERROR("IndexRegistry is required for DatedOISHelper");
        }
        if (!point->overnight_index() || !point->overnight_index()->id()) {
            QUANTRA_INVALID_ARGUMENT("DatedOISHelper.overnight_index.id is required");
        }

        std::string indexId = point->overnight_index()->id()->str();

        // Hard error: projection_curve is not supported for DatedOISHelper
        if (point->deps() && point->deps()->projection_curve() &&
            point->deps()->projection_curve()->id()) {
            QUANTRA_INVALID_ARGUMENT(
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

        auto asof = QuantLib::Settings::instance().evaluationDate();
        auto forwardStart = (start - asof) * Days;
        auto tenor = (end - start) * Days;

        return std::make_shared<OISRateHelper>(
            point->settlement_days(),
            tenor,
            q,
            on,
            discount,
            false,
            0,
            ConventionToQL(point->fixed_leg_convention()),
            Frequency::Annual,
            CalendarToQL(point->calendar()),
            forwardStart,
            0.0,
            QuantLib::Pillar::LastRelevantDate,
            end,
            QuantLib::RateAveraging::Compound
        );
    }

    // ------------------------------------------------------------------
    // Stubs for basis / FX / XCCY
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_TenorBasisSwapHelper) {
        QUANTRA_NOT_IMPLEMENTED("TenorBasisSwapHelper: schema-ready but not implemented.");
    }
    else if (point_type == quantra::Point_FxSwapHelper) {
        QUANTRA_NOT_IMPLEMENTED("FxSwapHelper: schema-ready but not implemented.");
    }
    else if (point_type == quantra::Point_CrossCcyBasisHelper) {
        QUANTRA_NOT_IMPLEMENTED("CrossCcyBasisHelper: schema-ready but not implemented.");
    }

    QUANTRA_INVALID_ARGUMENT("Unknown point type: " + std::to_string(point_type));
    return nullptr;
}

} // namespace quantra
