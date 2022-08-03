// #include "pricing_parser.h"

// void PricingParser::parse(const quantra::Pricing *pricing)
// {
//     if (pricing == NULL)
//         QUANTRA_ERROR("Pricing not found");

//     this->parse_term_structure(pricing->curves());
// }

// void PricingParser::parse_term_structure(const flatbuffers::Vector<flatbuffers::Offset<quantra::TermStructure>> *curves)
// {
//     TermStructureParser term_structure_parser = TermStructureParser();
//     for (auto it = curves->begin(); it != curves->end(); it++)
//     {
//         std::shared_ptr<YieldTermStructure> term_structure = term_structure_parser.parse(*it);
//         this->term_structures.insert(std::make_pair(it->id()->str(), term_structure));
//     }
// }

// void PricingParser::parse_pricer(const flatbuffers::Vector<flatbuffers::Offset<quantra::Pricer>> *pricers)
// {
// }