#ifndef QUANTRASERVER_ZEROCOUPONBONDPARSER_H
#define QUANTRASERVER_ZEROCOUPONBONDPARSER_H

#include <ql/qldefines.hpp>
#include <ql/instruments/bond.hpp>
#include <ql/instruments/bonds/zerocouponbond.hpp>

#include "zero_coupon_bond_generated.h"
#include "enum_convert.h"
#include "date_convert.h"

using namespace QuantLib;
using namespace quantra;

class ZeroCouponBondParser
{

private:
public:
    // Returns a plain QuantLib::Bond built as a QuantLib::ZeroCouponBond.
    std::shared_ptr<QuantLib::Bond> parse(const quantra::ZeroCouponBond *bond);
};

#endif //QUANTRASERVER_ZEROCOUPONBONDPARSER_H
