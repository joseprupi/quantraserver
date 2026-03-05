#ifndef QUANTRA_EQUITY_OPTION_PARSER_H
#define QUANTRA_EQUITY_OPTION_PARSER_H

#include <memory>
#include <string>
#include <vector>

#include <ql/exercise.hpp>
#include <ql/instruments/barrieroption.hpp>
#include <ql/instruments/asianoption.hpp>
#include <ql/option.hpp>
#include <ql/payoff.hpp>

#include "equity_option_generated.h"

namespace quantra {

struct ParsedEquityOption {
    std::string tradeId;
    std::string underlyingId;
    double quantity = 1.0;
    quantra::enums::EquitySettlementType settlement = quantra::enums::EquitySettlementType_Physical;

    // Cached for diagnostics/reporting; payoff drives pricing.
    QuantLib::Option::Type optionType = QuantLib::Option::Call;
    double strike = 0.0;
    std::shared_ptr<QuantLib::Payoff> payoff;
    std::shared_ptr<QuantLib::Exercise> exercise;

    bool hasBarrier = false;
    QuantLib::Barrier::Type barrierType = QuantLib::Barrier::DownOut;
    double barrierLevel = 0.0;
    double rebate = 0.0;

    bool hasAsian = false;
    QuantLib::Average::Type asianAverageType = QuantLib::Average::Geometric;
    std::vector<QuantLib::Date> asianFixingDates;
    double asianRunningAccumulator = 0.0;
    int asianPastFixings = 0;
};

class EquityOptionParser {
public:
    ParsedEquityOption parse(const quantra::EquityOption* option) const;
};

} // namespace quantra

#endif // QUANTRA_EQUITY_OPTION_PARSER_H
