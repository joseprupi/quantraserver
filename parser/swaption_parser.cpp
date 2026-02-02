#include "swaption_parser.h"

std::shared_ptr<QuantLib::Swaption> SwaptionParser::parse(const quantra::Swaption *swaption)
{
    if (swaption == NULL)
        QUANTRA_ERROR("Swaption not found");

    if (swaption->underlying_swap() == NULL)
        QUANTRA_ERROR("Swaption underlying_swap not found");

    // Parse exercise date
    QuantLib::Date exerciseDate = DateToQL(swaption->exercise_date()->str());

    // Parse underlying swap
    VanillaSwapParser swapParser;
    swapParser.linkForwardingTermStructure(forwarding_term_structure_.currentLink());
    auto underlyingSwap = swapParser.parse(swaption->underlying_swap());

    // Create exercise based on type
    std::shared_ptr<QuantLib::Exercise> exercise;
    switch (swaption->exercise_type()) {
        case quantra::enums::ExerciseType_European:
            exercise = std::make_shared<QuantLib::EuropeanExercise>(exerciseDate);
            break;
        case quantra::enums::ExerciseType_Bermudan:
            // For now, treat as European - full Bermudan needs multiple dates
            exercise = std::make_shared<QuantLib::EuropeanExercise>(exerciseDate);
            break;
        case quantra::enums::ExerciseType_American:
            // American exercise from today to exercise date
            exercise = std::make_shared<QuantLib::AmericanExercise>(
                QuantLib::Settings::instance().evaluationDate(),
                exerciseDate
            );
            break;
        default:
            QUANTRA_ERROR("Invalid exercise type");
    }

    // Parse settlement type
    QuantLib::Settlement::Type settlementType;
    switch (swaption->settlement_type()) {
        case quantra::enums::SettlementType_Physical:
            settlementType = QuantLib::Settlement::Physical;
            break;
        case quantra::enums::SettlementType_Cash:
            settlementType = QuantLib::Settlement::Cash;
            break;
        default:
            settlementType = QuantLib::Settlement::Physical;
    }

    // Create swaption
    auto swaptionInstrument = std::make_shared<QuantLib::Swaption>(
        underlyingSwap,
        exercise,
        settlementType
    );

    return swaptionInstrument;
}

void SwaptionParser::linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure)
{
    forwarding_term_structure_.linkTo(term_structure);
}
