#include "term_structure_point_parser.h"

#include "request_validation.h"

#include <ql/experimental/termstructures/crosscurrencyratehelpers.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/instruments/makeois.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>

#include <sstream>

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
    double bump) const {
    if (quoteId && quotes) {
        const std::string id = quoteId->str();
        if (!id.empty()) {
            // The registry value already carries the curve bump (applied once
            // when the request's quotes were loaded), so it is returned as-is.
            return quotes->getHandle(id, expectedType);
        }
    }
    auto sq = std::make_shared<SimpleQuote>(inlineValue + bump);
    return Handle<Quote>(sq);
}

// =============================================================================
// Exogenous curve resolution
// =============================================================================

Handle<YieldTermStructure> TermStructurePointParser::resolveCurve(
    const quantra::CurveRef* ref, const CurveRegistry* curves) const {
    if (!ref || !ref->id()) return Handle<YieldTermStructure>();
    if (!curves) {
        QUANTRA_ERROR(
            "Curve dependency '" + ref->id()->str() + "' specified but CurveRegistry is null");
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
    double bump,
    const FxSpotRegistry* fxSpots) const {

    // A union whose type byte names a helper but whose value offset is absent
    // survives FlatBuffers verification; the static_casts in each branch below
    // would then dereference a null table and crash. Reject it up front.
    if (point_type != quantra::Point_NONE && data == nullptr) {
        QUANTRA_INVALID_ARGUMENT("Term structure point union type is set but its value is missing");
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
            q, requirePeriod(point->tenor(), "DepositHelper.tenor"),
            requireNonNegative(point->fixing_days(), "DepositHelper.fixing_days"),
            CalendarToQL(point->calendar().value()),
            ConventionToQL(point->business_day_convention().value()), true,
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
            q, point->months_to_start(), point->months_to_end(),
            requireNonNegative(point->fixing_days(), "FRAHelper.fixing_days"),
            CalendarToQL(point->calendar().value()),
            ConventionToQL(point->business_day_convention().value()), true,
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

        auto q =
            resolveQuote(priceValue, point->quote_id(), quotes, quantra::QuoteType_Curve, bump);

        if (!point->future_start_date()) {
            QUANTRA_INVALID_ARGUMENT("FutureHelper.future_start_date is required");
        }
        if (!point->calendar().has_value())
            QUANTRA_INVALID_ARGUMENT("FutureHelper.calendar is required");
        if (!point->business_day_convention().has_value())
            QUANTRA_INVALID_ARGUMENT("FutureHelper.business_day_convention is required");
        if (!point->day_counter().has_value())
            QUANTRA_INVALID_ARGUMENT("FutureHelper.day_counter is required");

        auto convexity =
            Handle<Quote>(std::make_shared<SimpleQuote>(point->convexity_adjustment()));

        return std::make_shared<FuturesRateHelper>(
            q, DateToQL(point->future_start_date()->str()), point->future_months(),
            CalendarToQL(point->calendar().value()),
            ConventionToQL(point->business_day_convention().value()), true,
            DayCounterToQL(point->day_counter().value()), convexity);
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
            q, requirePeriod(point->tenor(), "SwapHelper.tenor"),
            CalendarToQL(point->calendar().value()),
            FrequencyToQL(point->sw_fixed_leg_frequency().value()),
            ConventionToQL(point->sw_fixed_leg_convention().value()),
            DayCounterToQL(point->sw_fixed_leg_day_counter().value()), ibor, spread,
            point->fwd_start_days() * Days, discount);
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
            QUANTRA_INVALID_ARGUMENT("BondHelper requires one of price, rate or quote_id");
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
            q, requireNonNegative(point->settlement_days(), "BondHelper.settlement_days"),
            faceAmount, *schedule_parser.parse(point->schedule()), std::vector<Rate>(1, couponRate),
            DayCounterToQL(point->day_counter().value()),
            ConventionToQL(point->business_day_convention().value()), redemption,
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

        // Wire 0 = "no lookback". QuantLib encodes the off-state as
        // Null<Natural>, NOT as a zero-day lookback: an explicit 0 would force
        // the coupon fixing delay to 0 even when the index carries a non-zero
        // intrinsic fixing delay (different fixing dates, and it disables the
        // telescopic formula), so the two are not interchangeable.
        const int lookbackWire =
            requireNonNegativeInt(point->lookback_days(), "OISHelper.lookback_days");
        const Natural lookbackDays =
            lookbackWire == 0 ? Null<Natural>() : static_cast<Natural>(lookbackWire);

        // fixed_leg_frequency / fixed_leg_convention / calendar feed QuantLib's
        // PAYMENT frequency / convention / calendar slots (see the schema
        // doc-comments). fixed_leg_day_counter is accepted-but-unused: no
        // OISRateHelper overload takes a day counter.
        return std::make_shared<OISRateHelper>(
            requireNonNegativeInt(point->settlement_days(), "OISHelper.settlement_days"),
            requirePeriod(point->tenor(), "OISHelper.tenor"), q, on, discount,
            false, // telescopicValueDates: helper-internal performance toggle
            requireNonNegativeInt(point->payment_lag(), "OISHelper.payment_lag"),
            ConventionToQL(
                requireEnum(point->fixed_leg_convention(), "OISHelper.fixed_leg_convention")),
            FrequencyToQL(
                requireEnum(point->fixed_leg_frequency(), "OISHelper.fixed_leg_frequency")),
            CalendarToQL(requireEnum(point->calendar(), "OISHelper.calendar")),
            0 * Days, // forwardStart: spot-starting (tenor helper)
            0.0,      // overnightSpread
            QuantLib::Pillar::LastRelevantDate,
            Date(), // customPillarDate: unused with LastRelevantDate
            RateAveragingToQL(requireEnum(point->averaging_method(), "OISHelper.averaging_method")),
            ext::nullopt, // endOfMonth: QuantLib default
            ext::nullopt, // fixedPaymentFrequency: paymentFrequency drives both legs
            Calendar(),   // fixedCalendar: QuantLib default (index calendar)
            lookbackDays,
            static_cast<Natural>(
                requireNonNegativeInt(point->lockout_days(), "OISHelper.lockout_days")),
            requireBool(point->apply_observation_shift(), "OISHelper.apply_observation_shift"));
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
        Date end = DateToQL(point->end_date()->str());

        // Resolve exogenous discount curve
        Handle<YieldTermStructure> discount;
        if (point->deps() && point->deps()->discount_curve()) {
            discount = resolveCurve(point->deps()->discount_curve(), curves);
        }

        auto asof = QuantLib::Settings::instance().evaluationDate();
        auto forwardStart = (start - asof) * Days;
        auto tenor = (end - start) * Days;

        // Wire 0 = "no lookback"; QuantLib's off-state is Null<Natural>, not a
        // zero-day lookback (see the OISHelper branch above for why the two
        // differ when the index has a non-zero intrinsic fixing delay).
        const int lookbackWire =
            requireNonNegativeInt(point->lookback_days(), "DatedOISHelper.lookback_days");
        const Natural lookbackDays =
            lookbackWire == 0 ? Null<Natural>() : static_cast<Natural>(lookbackWire);

        // fixed_leg_frequency / fixed_leg_convention / calendar feed QuantLib's
        // PAYMENT frequency / convention / calendar slots (see the schema
        // doc-comments). fixed_leg_day_counter is accepted-but-unused: no
        // OISRateHelper overload takes a day counter.
        return std::make_shared<OISRateHelper>(
            requireNonNegativeInt(point->settlement_days(), "DatedOISHelper.settlement_days"),
            tenor, q, on, discount,
            false, // telescopicValueDates: helper-internal performance toggle
            requireNonNegativeInt(point->payment_lag(), "DatedOISHelper.payment_lag"),
            ConventionToQL(
                requireEnum(point->fixed_leg_convention(), "DatedOISHelper.fixed_leg_convention")),
            FrequencyToQL(
                requireEnum(point->fixed_leg_frequency(), "DatedOISHelper.fixed_leg_frequency")),
            CalendarToQL(requireEnum(point->calendar(), "DatedOISHelper.calendar")), forwardStart,
            0.0, // overnightSpread
            QuantLib::Pillar::LastRelevantDate, end,
            RateAveragingToQL(
                requireEnum(point->averaging_method(), "DatedOISHelper.averaging_method")),
            ext::nullopt, // endOfMonth: QuantLib default
            ext::nullopt, // fixedPaymentFrequency: paymentFrequency drives both legs
            Calendar(),   // fixedCalendar: QuantLib default (index calendar)
            lookbackDays,
            static_cast<Natural>(
                requireNonNegativeInt(point->lockout_days(), "DatedOISHelper.lockout_days")),
            requireBool(
                point->apply_observation_shift(), "DatedOISHelper.apply_observation_shift"));
    }

    // ------------------------------------------------------------------
    // FX swap helper (spot resolved from FxSpotRegistry, collateral curve
    // from CurveRegistry)
    //
    // QuantLib's FxSwapRateHelper reprices the forward S + points off two
    // discount curves: the collateral currency's curve (exogenous, given) and
    // the curve being bootstrapped (the other currency's discount curve,
    // attached by setTermStructure). The spot lag, the joint settlement
    // calendar and the business-day convention are the spot's own: the pair's
    // conventions live on FxSpotSpec once, not on every pillar.
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_FxSwapHelper) {
        auto point = static_cast<const quantra::FxSwapHelper*>(data);

        const bool has_quote = point->quote_id() && !point->quote_id()->str().empty();
        double pointsValue;
        if (point->fx_points().has_value() && !has_quote) {
            pointsValue = requireFinite(point->fx_points(), "FxSwapHelper.fx_points");
        } else if (has_quote && !point->fx_points().has_value()) {
            pointsValue = 0.0; // unused: quote_id supplies the points below
        } else {
            QUANTRA_INVALID_ARGUMENT("FxSwapHelper requires exactly one of fx_points or quote_id");
        }
        // Forward points move with the two rate curves, so they are curve
        // quotes and take the curve bump like every other pillar.
        auto points =
            resolveQuote(pointsValue, point->quote_id(), quotes, quantra::QuoteType_Curve, bump);

        const QuantLib::Period tenor = requirePeriod(point->tenor(), "FxSwapHelper.tenor");

        if (!point->fx_spot() || point->fx_spot()->str().empty()) {
            QUANTRA_INVALID_ARGUMENT("FxSwapHelper.fx_spot is required");
        }
        if (!fxSpots) {
            QUANTRA_ERROR("FxSpotRegistry is required for FxSwapHelper");
        }
        const FxSpot& spot = fxSpots->get(point->fx_spot()->str());

        const bool endOfMonth = requireBool(point->end_of_month(), "FxSwapHelper.end_of_month");

        if (!point->collateral_currency() || point->collateral_currency()->str().empty()) {
            QUANTRA_INVALID_ARGUMENT("FxSwapHelper.collateral_currency is required");
        }
        const std::string collateralCcy = point->collateral_currency()->str();
        bool baseIsCollateral;
        if (collateralCcy == spot.baseCurrency) {
            baseIsCollateral = true;
        } else if (collateralCcy == spot.quoteCurrency) {
            baseIsCollateral = false;
        } else {
            QUANTRA_INVALID_ARGUMENT(
                "FxSwapHelper.collateral_currency '" + collateralCcy +
                "' is neither currency of FX spot '" + spot.id + "' (" + spot.baseCurrency + "/" +
                spot.quoteCurrency + ")");
        }

        if (!point->collateral_curve() || !point->collateral_curve()->id() ||
            point->collateral_curve()->id()->str().empty()) {
            QUANTRA_INVALID_ARGUMENT("FxSwapHelper.collateral_curve.id is required");
        }
        Handle<YieldTermStructure> collateral = resolveCurve(point->collateral_curve(), curves);

        return std::make_shared<FxSwapRateHelper>(
            points, spot.rate, tenor, static_cast<Natural>(spot.spotDays), spot.settlementCalendar,
            spot.businessDayConvention, endOfMonth, baseIsCollateral, collateral);
    }

    // ------------------------------------------------------------------
    // Cross-currency basis swap helper (constant notional)
    //
    // QuantLib's ConstNotionalCrossCurrencyBasisSwapRateHelper builds two
    // floating legs (notional 1 in each currency, exchanged at start and end)
    // from the two indices AS GIVEN: their forwarding curves must be attached
    // here, so both are explicit, exogenous references. The leg whose
    // discount curve is absent is discounted with the curve being
    // bootstrapped; the other leg with the given (collateral) curve. The spot
    // supplies the settlement lag and names the pair; its rate cancels from
    // a constant-notional par condition and is not passed to QuantLib.
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_CrossCcyBasisHelper) {
        auto point = static_cast<const quantra::CrossCcyBasisHelper*>(data);

        const bool has_quote = point->quote_id() && !point->quote_id()->str().empty();
        double spreadValue;
        if (point->spread().has_value() && !has_quote) {
            spreadValue = requireFinite(point->spread(), "CrossCcyBasisHelper.spread");
        } else if (has_quote && !point->spread().has_value()) {
            spreadValue = 0.0; // unused: quote_id supplies the spread below
        } else {
            QUANTRA_INVALID_ARGUMENT(
                "CrossCcyBasisHelper requires exactly one of spread or quote_id");
        }
        // The basis moves with the two rate curves: a curve quote, bumped
        // like every other pillar.
        auto basis =
            resolveQuote(spreadValue, point->quote_id(), quotes, quantra::QuoteType_Curve, bump);

        const QuantLib::Period tenor = requirePeriod(point->tenor(), "CrossCcyBasisHelper.tenor");

        if (!point->fx_spot() || point->fx_spot()->str().empty()) {
            QUANTRA_INVALID_ARGUMENT("CrossCcyBasisHelper.fx_spot is required");
        }
        if (!fxSpots) {
            QUANTRA_ERROR("FxSpotRegistry is required for CrossCcyBasisHelper");
        }
        const FxSpot& spot = fxSpots->get(point->fx_spot()->str());

        if (!indices) {
            QUANTRA_ERROR("IndexRegistry is required for CrossCcyBasisHelper");
        }

        // One leg: index (in the leg's currency) cloned onto its forwarding
        // curve. Both registry index types derive from IborIndex; an
        // OvernightIndex keeps its dynamic type through clone(), which is how
        // QuantLib tells a compounded overnight leg from an IBOR leg.
        auto resolveLeg = [&](const quantra::IndexRef* ref, const quantra::CurveRef* forwarding,
                              const std::string& legCurrency, const char* indexField,
                              const char* curveField, const char* currencyField) {
            if (!ref || !ref->id() || ref->id()->str().empty()) {
                QUANTRA_INVALID_ARGUMENT(
                    std::string("CrossCcyBasisHelper.") + indexField + ".id is required");
            }
            const std::string indexId = ref->id()->str();
            auto bare = std::dynamic_pointer_cast<IborIndex>(indices->get(indexId));
            if (!bare) {
                QUANTRA_INVALID_ARGUMENT(
                    std::string("CrossCcyBasisHelper.") + indexField + " '" + indexId +
                    "' is not an IBOR or overnight index");
            }
            if (bare->currency().code() != legCurrency) {
                QUANTRA_INVALID_ARGUMENT(
                    std::string("CrossCcyBasisHelper.") + indexField + " '" + indexId + "' is in " +
                    bare->currency().code() + " but the " + currencyField + " of FX spot '" +
                    spot.id + "' is " + legCurrency);
            }
            if (!forwarding || !forwarding->id() || forwarding->id()->str().empty()) {
                QUANTRA_INVALID_ARGUMENT(
                    std::string("CrossCcyBasisHelper.") + curveField + ".id is required");
            }
            Handle<YieldTermStructure> curve = resolveCurve(forwarding, curves);
            return bare->clone(curve);
        };
        std::shared_ptr<IborIndex> baseIdx = resolveLeg(
            point->base_index(), point->base_forwarding_curve(), spot.baseCurrency, "base_index",
            "base_forwarding_curve", "base_currency");
        std::shared_ptr<IborIndex> quoteIdx = resolveLeg(
            point->quote_index(), point->quote_forwarding_curve(), spot.quoteCurrency,
            "quote_index", "quote_forwarding_curve", "quote_currency");

        // Exactly one discount curve is given: the collateral side. The other
        // leg is discounted with the curve being bootstrapped.
        auto hasRef = [](const quantra::CurveRef* ref) {
            return ref && ref->id() && !ref->id()->str().empty();
        };
        const bool baseGiven = hasRef(point->base_discount_curve());
        const bool quoteGiven = hasRef(point->quote_discount_curve());
        if (baseGiven == quoteGiven) {
            QUANTRA_INVALID_ARGUMENT(
                "CrossCcyBasisHelper requires exactly one of base_discount_curve or "
                "quote_discount_curve: the given one is the collateral currency's discount "
                "curve, the absent one is the curve being bootstrapped");
        }
        Handle<YieldTermStructure> collateral = resolveCurve(
            baseGiven ? point->base_discount_curve() : point->quote_discount_curve(), curves);
        const bool baseIsCollateral = baseGiven;

        const bool spreadOnBase =
            requireBool(point->spread_on_base_leg(), "CrossCcyBasisHelper.spread_on_base_leg");

        // Payment frequency: QuantLib needs an explicit coupon schedule for a
        // compounded overnight leg and pays an IBOR leg at the index tenor.
        const bool baseOvernight = std::dynamic_pointer_cast<OvernightIndex>(baseIdx) != nullptr;
        const bool quoteOvernight = std::dynamic_pointer_cast<OvernightIndex>(quoteIdx) != nullptr;
        Frequency paymentFrequency = NoFrequency;
        if (baseOvernight || quoteOvernight) {
            if (!point->payment_frequency().has_value()) {
                QUANTRA_INVALID_ARGUMENT(
                    "CrossCcyBasisHelper.payment_frequency is required when base_index or "
                    "quote_index is an overnight index (coupon frequency of the compounded "
                    "overnight leg)");
            }
            paymentFrequency = FrequencyToQL(point->payment_frequency().value());
            if (paymentFrequency == NoFrequency || paymentFrequency == Once ||
                paymentFrequency == OtherFrequency) {
                QUANTRA_INVALID_ARGUMENT(
                    "CrossCcyBasisHelper.payment_frequency must be a periodic frequency "
                    "(Annual, Semiannual, Quarterly, Monthly, ...)");
            }
            auto checkIborTenor = [&](const std::shared_ptr<IborIndex>& ibor, bool overnight,
                                      const char* field) {
                if (overnight) return;
                if (QuantLib::Period(paymentFrequency) != ibor->tenor()) {
                    std::ostringstream os;
                    os << "CrossCcyBasisHelper.payment_frequency " << paymentFrequency
                       << " does not match the tenor " << ibor->tenor() << " of " << field << " '"
                       << ibor->familyName() << "': an IBOR leg pays at its index tenor";
                    QUANTRA_INVALID_ARGUMENT(os.str());
                }
            };
            checkIborTenor(baseIdx, baseOvernight, "base_index");
            checkIborTenor(quoteIdx, quoteOvernight, "quote_index");
        } else if (point->payment_frequency().has_value()) {
            QUANTRA_INVALID_ARGUMENT(
                "CrossCcyBasisHelper.payment_frequency must be absent when both indices are "
                "IBOR: each leg pays at its index tenor");
        }

        const int paymentLag =
            requireNonNegativeInt(point->payment_lag(), "CrossCcyBasisHelper.payment_lag");
        const QuantLib::Calendar calendar =
            CalendarToQL(requireEnum(point->calendar(), "CrossCcyBasisHelper.calendar"));
        const BusinessDayConvention convention = ConventionToQL(requireEnum(
            point->business_day_convention(), "CrossCcyBasisHelper.business_day_convention"));
        const bool endOfMonth =
            requireBool(point->end_of_month(), "CrossCcyBasisHelper.end_of_month");

        if (requireBool(point->mtm_reset(), "CrossCcyBasisHelper.mtm_reset")) {
            QUANTRA_INVALID_ARGUMENT(
                "CrossCcyBasisHelper.mtm_reset = true is not supported in this version "
                "(resetting-notional cross-currency swaps); use false");
        }

        return std::make_shared<ConstNotionalCrossCurrencyBasisSwapRateHelper>(
            basis, tenor, static_cast<Natural>(spot.spotDays), calendar, convention, endOfMonth,
            baseIdx, quoteIdx, collateral, baseIsCollateral, spreadOnBase, paymentFrequency,
            paymentLag);
    }

    // ------------------------------------------------------------------
    // Stub: tenor basis (schema-ready, not built)
    // ------------------------------------------------------------------
    else if (point_type == quantra::Point_TenorBasisSwapHelper) {
        QUANTRA_NOT_IMPLEMENTED("TenorBasisSwapHelper: schema-ready but not implemented.");
    }

    QUANTRA_INVALID_ARGUMENT("Unknown point type: " + std::to_string(point_type));
    return nullptr;
}

} // namespace quantra
