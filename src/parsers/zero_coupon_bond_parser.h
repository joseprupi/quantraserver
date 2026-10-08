#ifndef QUANTRASERVER_ZEROCOUPONBONDPARSER_H
#define QUANTRASERVER_ZEROCOUPONBONDPARSER_H

#include "date_convert.h"
#include "enum_convert.h"

#include "zero_coupon_bond_generated.h"

#include <ql/instruments/bond.hpp>
#include <ql/instruments/bonds/zerocouponbond.hpp>
#include <ql/qldefines.hpp>

class ZeroCouponBondParser {

private:
public:
    // Returns a plain QuantLib::Bond built as a QuantLib::ZeroCouponBond.
    std::shared_ptr<QuantLib::Bond> parse(const quantra::ZeroCouponBond* bond);
};

#endif //QUANTRASERVER_ZEROCOUPONBONDPARSER_H
