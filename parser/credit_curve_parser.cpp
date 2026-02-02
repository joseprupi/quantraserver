#include "credit_curve_parser.h"

std::shared_ptr<QuantLib::DefaultProbabilityTermStructure> CreditCurveParser::parse(
    const quantra::CreditCurve *credit_curve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve)
{
    if (credit_curve == NULL)
        QUANTRA_ERROR("CreditCurve not found");

    QuantLib::Date referenceDate = DateToQL(credit_curve->reference_date()->str());
    QuantLib::Calendar calendar = CalendarToQL(credit_curve->calendar());
    QuantLib::DayCounter dayCounter = DayCounterToQL(credit_curve->day_counter());
    double recoveryRate = credit_curve->recovery_rate();

    auto quotes = credit_curve->quotes();
    
    // If flat_hazard_rate is provided or no quotes, use FlatHazardRate
    double flatHazardRate = credit_curve->flat_hazard_rate();
    if (flatHazardRate > 0.0 || quotes == NULL || quotes->size() == 0)
    {
        // Use flat hazard rate (default to 1% if nothing provided)
        double hazardRate = flatHazardRate > 0.0 ? flatHazardRate : 0.01;
        return std::make_shared<QuantLib::FlatHazardRate>(
            referenceDate,
            hazardRate,
            dayCounter
        );
    }

    // Build from CDS spread quotes
    std::vector<std::shared_ptr<QuantLib::DefaultProbabilityHelper>> helpers;

    for (size_t i = 0; i < quotes->size(); i++)
    {
        auto quote = quotes->Get(i);
        QuantLib::Period tenor = quote->tenor_number() * TimeUnitToQL(quote->tenor_time_unit());
        
        auto spreadQuote = std::make_shared<QuantLib::SimpleQuote>(quote->spread());
        
        auto helper = std::make_shared<QuantLib::SpreadCdsHelper>(
            QuantLib::Handle<QuantLib::Quote>(spreadQuote),
            tenor,
            0,  // settlement days
            calendar,
            QuantLib::Quarterly,
            QuantLib::Following,
            QuantLib::DateGeneration::TwentiethIMM,
            dayCounter,
            recoveryRate,
            discountCurve
        );
        
        helpers.push_back(helper);
    }

    // Bootstrap the curve
    auto creditCurve = std::make_shared<QuantLib::PiecewiseDefaultCurve<QuantLib::SurvivalProbability, QuantLib::LogLinear>>(
        referenceDate,
        helpers,
        dayCounter
    );

    return creditCurve;
}
