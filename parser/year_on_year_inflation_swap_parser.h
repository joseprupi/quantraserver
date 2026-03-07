#ifndef QUANTRASERVER_YEARONYEARINFLATIONSWAPPARSER_H
#define QUANTRASERVER_YEARONYEARINFLATIONSWAPPARSER_H

#include <ql/instruments/yearonyearinflationswap.hpp>

#include "year_on_year_inflation_swap_generated.h"

class YearOnYearInflationSwapParser {
public:
    std::shared_ptr<QuantLib::YearOnYearInflationSwap> parse(
        const quantra::YearOnYearInflationSwap* swap,
        const std::shared_ptr<QuantLib::YoYInflationIndex>& inflationIndex);
};

#endif // QUANTRASERVER_YEARONYEARINFLATIONSWAPPARSER_H
