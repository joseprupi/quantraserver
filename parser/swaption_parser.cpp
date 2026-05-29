#include "swaption_parser.h"

#include <vector>

std::shared_ptr<QuantLib::Swaption> SwaptionParser::parse(
    const quantra::Swaption *swaption,
    const quantra::IndexRegistry& indices)
{
    if (swaption == NULL)
        QUANTRA_INVALID_ARGUMENT("Swaption not found");

    const auto exerciseType = swaption->exercise_type();
    const bool needsSingleExerciseDate =
        (exerciseType == quantra::enums::ExerciseType_European ||
         exerciseType == quantra::enums::ExerciseType_American);
    const bool needsExerciseDateSet =
        (exerciseType == quantra::enums::ExerciseType_Bermudan);

    if (needsSingleExerciseDate && swaption->exercise_date() == NULL)
        QUANTRA_INVALID_ARGUMENT("Swaption exercise_date not found");

    if (needsExerciseDateSet) {
        if (swaption->exercise_dates() == NULL || swaption->exercise_dates()->size() < 2) {
            QUANTRA_INVALID_ARGUMENT("Swaption Bermudan requires exercise_dates with at least 2 dates");
        }
    }

    // Parse exercise date(s)
    QuantLib::Date exerciseDate;
    if (swaption->exercise_date() != NULL) {
        exerciseDate = DateToQL(swaption->exercise_date()->str());
    }
    std::vector<QuantLib::Date> bermudanExerciseDates;
    if (swaption->exercise_dates() != NULL) {
        bermudanExerciseDates.reserve(swaption->exercise_dates()->size());
        for (flatbuffers::uoffset_t i = 0; i < swaption->exercise_dates()->size(); ++i) {
            auto* exDate = swaption->exercise_dates()->Get(i);
            if (!exDate) {
                QUANTRA_INVALID_ARGUMENT("Swaption exercise_dates contains null entry");
            }
            bermudanExerciseDates.push_back(DateToQL(exDate->str()));
        }
    }
    if (needsExerciseDateSet) {
        const QuantLib::Date evalDate = QuantLib::Settings::instance().evaluationDate();
        for (size_t i = 1; i < bermudanExerciseDates.size(); ++i) {
            if (bermudanExerciseDates[i] <= bermudanExerciseDates[i - 1]) {
                QUANTRA_INVALID_ARGUMENT("Swaption Bermudan exercise_dates must be strictly increasing");
            }
        }
        for (const auto& d : bermudanExerciseDates) {
            if (d < evalDate) {
                QUANTRA_INVALID_ARGUMENT("Swaption Bermudan exercise_dates must be on/after evaluation date");
            }
        }
    }

    // Parse underlying swap (passing indices for floating leg resolution)
    std::shared_ptr<QuantLib::FixedVsFloatingSwap> underlyingSwap;
    switch (swaption->underlying_type()) {
        case quantra::SwaptionUnderlying_VanillaSwap: {
            auto underlying = swaption->underlying_as_VanillaSwap();
            if (!underlying) QUANTRA_INVALID_ARGUMENT("Swaption underlying VanillaSwap not found");
            VanillaSwapParser swapParser;
            swapParser.linkForwardingTermStructure(forwarding_term_structure_.currentLink());
            underlyingSwap = swapParser.parse(underlying, indices);
            break;
        }
        case quantra::SwaptionUnderlying_OisSwap: {
            auto underlying = swaption->underlying_as_OisSwap();
            if (!underlying) QUANTRA_INVALID_ARGUMENT("Swaption underlying OisSwap not found");
            OisSwapParser swapParser;
            swapParser.linkForwardingTermStructure(forwarding_term_structure_.currentLink());
            underlyingSwap = swapParser.parse(underlying, indices);
            break;
        }
        case quantra::SwaptionUnderlying_NONE:
            QUANTRA_INVALID_ARGUMENT("Swaption underlying not found");
        default:
            QUANTRA_INVALID_ARGUMENT("Invalid swaption underlying type");
    }

    // Create exercise based on type
    std::shared_ptr<QuantLib::Exercise> exercise;
    switch (swaption->exercise_type()) {
        case quantra::enums::ExerciseType_European:
            exercise = std::make_shared<QuantLib::EuropeanExercise>(exerciseDate);
            break;
        case quantra::enums::ExerciseType_Bermudan:
            exercise = std::make_shared<QuantLib::BermudanExercise>(bermudanExerciseDates);
            break;
        case quantra::enums::ExerciseType_American:
            exercise = std::make_shared<QuantLib::AmericanExercise>(
                QuantLib::Settings::instance().evaluationDate(),
                exerciseDate
            );
            break;
        default:
            QUANTRA_INVALID_ARGUMENT("Invalid exercise type");
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
