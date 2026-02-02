#ifndef QUANTRASERVER_CREDIT_CURVE_PARSER_H
#define QUANTRASERVER_CREDIT_CURVE_PARSER_H

#include <ql/qldefines.hpp>
#include <ql/termstructures/credit/defaultprobabilityhelpers.hpp>
#include <ql/termstructures/credit/piecewisedefaultcurve.hpp>
#include <ql/termstructures/credit/flathazardrate.hpp>

#include "credit_curve_generated.h"
#include "enums.h"
#include "common.h"

using namespace QuantLib;

// =============================================================================
// CREDIT CURVE PARSER
// =============================================================================
// Parses FlatBuffers CreditCurve into QuantLib DefaultProbabilityTermStructure.

class CreditCurveParser
{
public:
    std::shared_ptr<QuantLib::DefaultProbabilityTermStructure> parse(
        const quantra::CreditCurve *credit_curve,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve);
};

#endif // QUANTRASERVER_CREDIT_CURVE_PARSER_H
