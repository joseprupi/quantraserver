#include "fra_parser.h"

std::shared_ptr<QuantLib::ForwardRateAgreement> FRAParser::parse(const quantra::FRA *fra)
{
    if (fra == NULL)
        QUANTRA_ERROR("FRA not found");

    if (fra->index() == NULL)
        QUANTRA_ERROR("FRA index not found");

    // Parse dates
    Date startDate = DateToQL(fra->start_date()->str());
    Date maturityDate = DateToQL(fra->maturity_date()->str());

    // Parse FRA position type
    QuantLib::Position::Type position;
    switch (fra->fra_type()) {
        case quantra::enums::FRAType_Long:
            position = QuantLib::Position::Long;
            break;
        case quantra::enums::FRAType_Short:
            position = QuantLib::Position::Short;
            break;
        default:
            QUANTRA_ERROR("Invalid FRA type");
    }

    // Parse index
    IndexParser indexParser;
    indexParser.link_term_structure(forwarding_term_structure_.currentLink());
    auto iborIndex = indexParser.parse(fra->index());

    // Create the FRA
    auto fraInstrument = std::make_shared<QuantLib::ForwardRateAgreement>(
        startDate,
        maturityDate,
        position,
        fra->strike(),
        fra->notional(),
        iborIndex,
        forwarding_term_structure_
    );

    return fraInstrument;
}

void FRAParser::linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure)
{
    forwarding_term_structure_.linkTo(term_structure);
}
