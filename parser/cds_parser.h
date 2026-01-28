#ifndef QUANTRASERVER_CDS_PARSER_H
#define QUANTRASERVER_CDS_PARSER_H

#include <ql/qldefines.hpp>
#include <ql/instruments/creditdefaultswap.hpp>
#include <ql/termstructures/credit/defaultprobabilityhelpers.hpp>
#include <ql/termstructures/credit/piecewisedefaultcurve.hpp>
#include <ql/termstructures/credit/flathazardrate.hpp>
#include <ql/time/schedule.hpp>

#include "cds_generated.h"
#include "price_cds_request_generated.h"
#include "enums.h"
#include "common.h"
#include "common_parser.h"

using namespace QuantLib;

class CDSParser
{
public:
    std::shared_ptr<QuantLib::CreditDefaultSwap> parse(const quantra::CDS *cds);
};

class CreditCurveParser
{
public:
    std::shared_ptr<QuantLib::DefaultProbabilityTermStructure> parse(
        const quantra::CreditCurve *credit_curve,
        const QuantLib::Date& referenceDate,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve);
};

#endif // QUANTRASERVER_CDS_PARSER_H
