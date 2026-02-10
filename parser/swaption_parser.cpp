#include "swaption_parser.h"

std::shared_ptr<QuantLib::Swaption> SwaptionParser::parse(
    const quantra::Swaption *swaption,
    const quantra::IndexRegistry& indices)
{
    if (swaption == NULL)
        QUANTRA_ERROR("Swaption not found");

    if (swaption->exercise_date() == NULL)
        QUANTRA_ERROR("Swaption exercise_date not found");

    // Parse exercise date
    QuantLib::Date exerciseDate = DateToQL(swaption->exercise_date()->str());

    // Parse underlying swap (passing indices for floating leg resolution)
    std::shared_ptr<QuantLib::FixedVsFloatingSwap> underlyingSwap;
    if (swaption->underlying_type() != quantra::SwaptionUnderlying_NONE) {
        switch (swaption->underlying_type()) {
            case quantra::SwaptionUnderlying_VanillaSwap: {
                auto underlying = swaption->underlying_as_VanillaSwap();
                if (!underlying) QUANTRA_ERROR("Swaption underlying VanillaSwap not found");
                VanillaSwapParser swapParser;
                swapParser.linkForwardingTermStructure(forwarding_term_structure_.currentLink());
                underlyingSwap = swapParser.parse(underlying, indices);
                break;
            }
            case quantra::SwaptionUnderlying_OisSwap: {
                auto underlying = swaption->underlying_as_OisSwap();
                if (!underlying) QUANTRA_ERROR("Swaption underlying OisSwap not found");
                OisSwapParser swapParser;
                swapParser.linkForwardingTermStructure(forwarding_term_structure_.currentLink());
                underlyingSwap = swapParser.parse(underlying, indices);
                break;
            }
            default:
                QUANTRA_ERROR("Invalid swaption underlying type");
        }
    } else if (swaption->underlying_swap() != NULL) {
        // Legacy field: underlying_swap
        VanillaSwapParser swapParser;
        swapParser.linkForwardingTermStructure(forwarding_term_structure_.currentLink());
        underlyingSwap = swapParser.parse(swaption->underlying_swap(), indices);
    } else {
        QUANTRA_ERROR("Swaption underlying not found");
    }

    // Create exercise based on type
    std::shared_ptr<QuantLib::Exercise> exercise;
    switch (swaption->exercise_type()) {
        case quantra::enums::ExerciseType_European:
            exercise = std::make_shared<QuantLib::EuropeanExercise>(exerciseDate);
            break;
        case quantra::enums::ExerciseType_Bermudan:
            exercise = std::make_shared<QuantLib::EuropeanExercise>(exerciseDate);
            break;
        case quantra::enums::ExerciseType_American:
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
    QuantLib::Settlement::Method settlementMethod =
        SettlementMethodToQL(swaption->settlement_method());

    auto swaptionInstrument = std::make_shared<QuantLib::Swaption>(
        underlyingSwap,
        exercise,
        settlementType,
        settlementMethod
    );

    return swaptionInstrument;
}

void SwaptionParser::linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure)
{
    forwarding_term_structure_.linkTo(term_structure);
}
