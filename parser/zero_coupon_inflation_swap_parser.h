#ifndef QUANTRASERVER_ZEROCOUPONINFLATIONSWAPPARSER_H
#define QUANTRASERVER_ZEROCOUPONINFLATIONSWAPPARSER_H

#include <ql/instruments/zerocouponinflationswap.hpp>

#include "zero_coupon_inflation_swap_generated.h"

class ZeroCouponInflationSwapParser {
public:
    std::shared_ptr<QuantLib::ZeroCouponInflationSwap> parse(
        const quantra::ZeroCouponInflationSwap* swap,
        const std::shared_ptr<QuantLib::ZeroInflationIndex>& inflationIndex);
};

#endif // QUANTRASERVER_ZEROCOUPONINFLATIONSWAPPARSER_H
