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
    // Note: QuantLib CDS constructor signature:
    // CreditDefaultSwap(Protection::Side side, Real notional, Rate spread,
    //                   const Schedule& schedule, BusinessDayConvention convention,
    //                   const DayCounter& dayCounter,
    //                   bool settlesAccrual = true, bool paysAtDefaultTime = true,
    //                   const Date& protectionStart = Date(),
    //                   const ext::shared_ptr<Claim>& claim = ext::shared_ptr<Claim>(),
    //                   const DayCounter& lastPeriodDayCounter = DayCounter(),
    //                   bool rebatesAccrual = true,
    //                   const Date& tradeDate = Date(),
    //                   Natural cashSettlementDays = 3)

    return std::make_shared<QuantLib::CreditDefaultSwap>(
        side,
        cds->notional(),
        cds->spread(),
        *schedule,
        ConventionToQL(cds->business_day_convention()),
        DayCounterToQL(cds->day_counter()),
        true,   // settlesAccrual
        true    // paysAtDefaultTime
    );
}

std::shared_ptr<QuantLib::DefaultProbabilityTermStructure> CreditCurveParser::parse(
    const quantra::CreditCurve *credit_curve,
    const QuantLib::Date& referenceDate,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve)
{
    if (credit_curve == NULL)
        QUANTRA_ERROR("CreditCurve not found");

    double recoveryRate = credit_curve->recovery_rate();
    double flatHazardRate = credit_curve->flat_hazard_rate();
    auto quotes = credit_curve->quotes();

    // If flat hazard rate is provided or no quotes, use FlatHazardRate
    if (flatHazardRate > 0.0 || quotes == NULL || quotes->size() == 0)
    {
        // Use flat hazard rate
        double hazardRate = flatHazardRate > 0.0 ? flatHazardRate : 0.01;  // Default 1%
        return std::make_shared<QuantLib::FlatHazardRate>(
            referenceDate,
            hazardRate,
            QuantLib::Actual365Fixed()
        );
    }

    // Build from CDS spread quotes using SpreadCdsHelper
    std::vector<std::shared_ptr<QuantLib::DefaultProbabilityHelper>> helpers;

    for (size_t i = 0; i < quotes->size(); i++)
    {
        auto quote = quotes->Get(i);
        QuantLib::Period tenor(quote->tenor_number(), TimeUnitToQL(quote->tenor_time_unit()));
        
        auto spreadQuote = std::make_shared<QuantLib::SimpleQuote>(quote->spread());
        
        auto helper = std::make_shared<QuantLib::SpreadCdsHelper>(
            QuantLib::Handle<QuantLib::Quote>(spreadQuote),
            tenor,
            0,  // settlement days
            QuantLib::TARGET(),
            QuantLib::Quarterly,
            QuantLib::Following,
            QuantLib::DateGeneration::TwentiethIMM,
            QuantLib::Actual365Fixed(),
            recoveryRate,
            discountCurve
        );
        
        helpers.push_back(helper);
    }

    // Bootstrap the survival probability curve
    return std::make_shared<QuantLib::PiecewiseDefaultCurve<QuantLib::SurvivalProbability, QuantLib::LogLinear>>(
        referenceDate,
        helpers,
        QuantLib::Actual365Fixed()
    );
}
