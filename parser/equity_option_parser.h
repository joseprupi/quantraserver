#ifndef QUANTRA_EQUITY_OPTION_PARSER_H
#define QUANTRA_EQUITY_OPTION_PARSER_H

#include <memory>
#include <string>

#include <ql/exercise.hpp>
#include <ql/instruments/barrieroption.hpp>
#include <ql/option.hpp>

#include "equity_option_generated.h"

namespace quantra {

struct ParsedEquityOption {
    std::string tradeId;
    std::string underlyingId;
    double quantity = 1.0;
    quantra::enums::EquitySettlementType settlement = quantra::enums::EquitySettlementType_Physical;

    QuantLib::Option::Type optionType = QuantLib::Option::Call;
    double strike = 0.0;
    std::shared_ptr<QuantLib::Exercise> exercise;

    bool hasBarrier = false;
    QuantLib::Barrier::Type barrierType = QuantLib::Barrier::DownOut;
    double barrierLevel = 0.0;
    double rebate = 0.0;
};

class EquityOptionParser {
public:
    ParsedEquityOption parse(const quantra::EquityOption* option) const;
};

} // namespace quantra

#endif // QUANTRA_EQUITY_OPTION_PARSER_H
