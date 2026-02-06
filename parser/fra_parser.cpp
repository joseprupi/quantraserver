#include "fra_parser.h"

std::shared_ptr<QuantLib::ForwardRateAgreement> FRAParser::parse(
    const quantra::FRA *fra,
    const quantra::IndexRegistry& indices)
{
    if (fra == NULL)
        QUANTRA_ERROR("FRA not found");

    if (!fra->index() || !fra->index()->id())
        QUANTRA_ERROR("FRA index.id is required");

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

    // Resolve index from registry and clone with forwarding curve
    std::string indexId = fra->index()->id()->str();
    auto iborIndex = indices.getIborWithCurve(indexId, forwarding_term_structure_);

    auto fraInstrument = std::make_shared<QuantLib::ForwardRateAgreement>(
        iborIndex,
        startDate,
        maturityDate,
        position,
        fra->strike(),
        fra->notional(),
        forwarding_term_structure_
    );

    return fraInstrument;
}

void FRAParser::linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure)
{
    forwarding_term_structure_.linkTo(term_structure);
}
