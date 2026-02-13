#include "cds_parser.h"

std::shared_ptr<QuantLib::CreditDefaultSwap> CDSParser::parse(const quantra::CDS *cds)
{
    if (cds == NULL)
        QUANTRA_ERROR("CDS not found");

    ScheduleParser schedule_parser;
    auto schedule = schedule_parser.parse(cds->schedule());

    // Determine protection side
    QuantLib::Protection::Side side;
    switch (cds->side()) {
        case quantra::enums::ProtectionSide_Buyer:
            side = QuantLib::Protection::Buyer;
            break;
        case quantra::enums::ProtectionSide_Seller:
            side = QuantLib::Protection::Seller;
            break;
        default:
            side = QuantLib::Protection::Buyer;
    }

    // Create the CDS instrument
    // Note: QuantLib CDS constructor signatures:
    // Running-spread only:
    //   CreditDefaultSwap(Protection::Side side, Real notional, Rate spread,
    //                     const Schedule& schedule, BusinessDayConvention convention,
    //                     const DayCounter& dayCounter, bool settlesAccrual = true,
    //                     bool paysAtDefaultTime = true, const Date& protectionStart = Date(),
    //                     const ext::shared_ptr<Claim>& claim = ext::shared_ptr<Claim>(),
    //                     const DayCounter& lastPeriodDayCounter = DayCounter(),
    //                     bool rebatesAccrual = true, const Date& tradeDate = Date(),
    //                     Natural cashSettlementDays = 3)
    // Upfront + running spread:
    //   CreditDefaultSwap(Protection::Side side, Real notional, Rate upfront, Rate spread,
    //                     const Schedule& schedule, BusinessDayConvention convention,
    //                     const DayCounter& dayCounter, bool settlesAccrual = true,
    //                     bool paysAtDefaultTime = true, const Date& protectionStart = Date(),
    //                     const Date& upfrontDate = Date(),
    //                     const ext::shared_ptr<Claim>& claim = ext::shared_ptr<Claim>(),
    //                     const DayCounter& lastPeriodDayCounter = DayCounter(),
    //                     bool rebatesAccrual = true, const Date& tradeDate = Date(),
    //                     Natural cashSettlementDays = 3)

    bool settlesAccrual = cds->settles_accrual();
    bool paysAtDefaultTime = cds->pays_at_default_time();
    bool rebatesAccrual = cds->rebates_accrual();
    DayCounter lastPeriodDc = DayCounterToQL(cds->last_period_day_counter());

    Date protectionStart;
    if (cds->protection_start() && cds->protection_start()->size() > 0) {
        protectionStart = DateToQL(cds->protection_start()->str());
    }

    Date upfrontDate;
    if (cds->upfront_date() && cds->upfront_date()->size() > 0) {
        upfrontDate = DateToQL(cds->upfront_date()->str());
    }

    Date tradeDate;
    if (cds->trade_date() && cds->trade_date()->size() > 0) {
        tradeDate = DateToQL(cds->trade_date()->str());
    }

    if (cds->upfront() != 0.0 || (cds->upfront_date() && cds->upfront_date()->size() > 0)) {
        return std::make_shared<QuantLib::CreditDefaultSwap>(
            side,
            cds->notional(),
            cds->upfront(),
            cds->running_coupon(),
            *schedule,
            ConventionToQL(cds->business_day_convention()),
            DayCounterToQL(cds->day_counter()),
            settlesAccrual,
            paysAtDefaultTime,
            protectionStart,
            upfrontDate,
            nullptr,
            lastPeriodDc,
            rebatesAccrual,
            tradeDate,
            cds->cash_settlement_days()
        );
    }

    return std::make_shared<QuantLib::CreditDefaultSwap>(
        side,
        cds->notional(),
        cds->running_coupon(),
        *schedule,
        ConventionToQL(cds->business_day_convention()),
        DayCounterToQL(cds->day_counter()),
        settlesAccrual,
        paysAtDefaultTime,
        protectionStart,
        nullptr,
        lastPeriodDc,
        rebatesAccrual,
        tradeDate,
        cds->cash_settlement_days()
    );
}

std::shared_ptr<QuantLib::DefaultProbabilityTermStructure> CreditCurveParser::parse(
    const quantra::CreditCurveSpec *credit_curve,
    const QuantLib::Date& referenceDate,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve)
{
    if (credit_curve == NULL)
        QUANTRA_ERROR("CreditCurveSpec not found");

    double recoveryRate = credit_curve->recovery_rate();
    double flatHazardRate = credit_curve->flat_hazard_rate();
    auto quotes = credit_curve->quotes();

    DayCounter curveDc = DayCounterToQL(credit_curve->day_counter());
    if (credit_curve->calendar() == quantra::enums::Calendar_NullCalendar) {
        QUANTRA_ERROR("CreditCurveSpec.calendar is required");
    }
    Calendar curveCal = CalendarToQL(credit_curve->calendar());

    const auto* helperConventions = credit_curve->helper_conventions();
    Frequency freq = FrequencyToQL(helperConventions ? helperConventions->frequency() : quantra::enums::Frequency_Quarterly);
    BusinessDayConvention bdc = ConventionToQL(
        helperConventions ? helperConventions->business_day_convention() : quantra::enums::BusinessDayConvention_Following);
    DateGeneration::Rule rule = DateGenerationToQL(
        helperConventions ? helperConventions->date_generation_rule() : quantra::enums::DateGenerationRule_TwentiethIMM);
    DayCounter lastPeriodDc = DayCounterToQL(
        helperConventions ? helperConventions->last_period_day_counter() : quantra::enums::DayCounter_Actual365Fixed);
    bool settlesAccrual = helperConventions ? helperConventions->settles_accrual() : true;
    bool paysAtDefaultTime = helperConventions ? helperConventions->pays_at_default_time() : true;
    bool rebatesAccrual = helperConventions ? helperConventions->rebates_accrual() : true;
    int settlementDays = helperConventions ? helperConventions->settlement_days() : 0;

    CreditDefaultSwap::PricingModel helperModel = CreditDefaultSwap::Midpoint;
    if (helperConventions) {
        switch (helperConventions->helper_model()) {
            case quantra::enums::CdsHelperModel_ISDA:
                helperModel = CreditDefaultSwap::ISDA;
                break;
            case quantra::enums::CdsHelperModel_MidPoint:
            default:
                helperModel = CreditDefaultSwap::Midpoint;
                break;
        }
    }

    // If flat hazard rate is provided or no quotes, use FlatHazardRate
    if (flatHazardRate > 0.0 || quotes == NULL || quotes->size() == 0)
    {
        // Use flat hazard rate
        double hazardRate = flatHazardRate > 0.0 ? flatHazardRate : 0.01;  // Default 1%
        return std::make_shared<QuantLib::FlatHazardRate>(
            referenceDate,
            hazardRate,
            curveDc
        );
    }

    // Build from CDS quotes using SpreadCdsHelper or UpfrontCdsHelper
    std::vector<std::shared_ptr<QuantLib::DefaultProbabilityHelper>> helpers;

    for (size_t i = 0; i < quotes->size(); i++)
    {
        auto quote = quotes->Get(i);
        QuantLib::Period tenor(quote->tenor_number(), TimeUnitToQL(quote->tenor_time_unit()));

        switch (quote->quote_type()) {
            case quantra::enums::CdsQuoteType_Upfront: {
                if (quote->running_coupon() == 0.0) {
                    QUANTRA_ERROR("CdsQuote.running_coupon is required for upfront quotes");
                }
                auto upfrontQuote = std::make_shared<QuantLib::SimpleQuote>(quote->quoted_upfront());
                auto helper = std::make_shared<QuantLib::UpfrontCdsHelper>(
                    QuantLib::Handle<QuantLib::Quote>(upfrontQuote),
                    quote->running_coupon(),
                    tenor,
                    settlementDays,
                    curveCal,
                    freq,
                    bdc,
                    rule,
                    curveDc,
                    recoveryRate,
                    discountCurve,
                    settlementDays,
                    settlesAccrual,
                    paysAtDefaultTime,
                    QuantLib::Date(),
                    lastPeriodDc,
                    rebatesAccrual,
                    helperModel
                );
                helpers.push_back(helper);
                break;
            }
            case quantra::enums::CdsQuoteType_ParSpread:
            default: {
                auto spreadQuote = std::make_shared<QuantLib::SimpleQuote>(quote->quoted_par_spread());
                auto helper = std::make_shared<QuantLib::SpreadCdsHelper>(
                    QuantLib::Handle<QuantLib::Quote>(spreadQuote),
                    tenor,
                    settlementDays,
                    curveCal,
                    freq,
                    bdc,
                    rule,
                    curveDc,
                    recoveryRate,
                    discountCurve,
                    settlesAccrual,
                    paysAtDefaultTime,
                    QuantLib::Date(),
                    lastPeriodDc,
                    rebatesAccrual,
                    helperModel
                );
                helpers.push_back(helper);
                break;
            }
        }
    }

    // Bootstrap the survival probability curve
    switch (credit_curve->curve_interpolator()) {
        case quantra::enums::Interpolator_Linear:
            return std::make_shared<QuantLib::PiecewiseDefaultCurve<QuantLib::SurvivalProbability, QuantLib::Linear>>(
                referenceDate, helpers, curveDc);
        case quantra::enums::Interpolator_BackwardFlat:
            return std::make_shared<QuantLib::PiecewiseDefaultCurve<QuantLib::SurvivalProbability, QuantLib::BackwardFlat>>(
                referenceDate, helpers, curveDc);
        case quantra::enums::Interpolator_LogLinear:
        default:
            return std::make_shared<QuantLib::PiecewiseDefaultCurve<QuantLib::SurvivalProbability, QuantLib::LogLinear>>(
                referenceDate, helpers, curveDc);
    }
}
