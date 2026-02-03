#ifndef QUANTRASERVER_QUOTE_PARSER_H
#define QUANTRASERVER_QUOTE_PARSER_H

#include <ql/handle.hpp>
#include <ql/quote.hpp>

#include "quotes_generated.h"
#include "common.h"

namespace quantra {

class QuoteParser {
public:
    QuantLib::Handle<QuantLib::Quote> parse(const quantra::QuoteSpec* q) const;
};

} // namespace quantra

#endif