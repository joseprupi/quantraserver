#include "quote_parser.h"

#include <ql/quotes/simplequote.hpp>

namespace quantra {

QuantLib::Handle<QuantLib::Quote> QuoteParser::parse(const quantra::QuoteSpec* q) const {
    if (!q) QUANTRA_ERROR("QuoteSpec not found");
    if (!q->id()) QUANTRA_ERROR("QuoteSpec.id required");

    auto sq = std::make_shared<QuantLib::SimpleQuote>(q->value());
    return QuantLib::Handle<QuantLib::Quote>(sq);
}

} // namespace quantra