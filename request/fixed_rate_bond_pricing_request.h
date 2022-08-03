#ifndef QUANTRASERVER_FIXEDRATEBONDPRICINGREQUEST_H
#define QUANTRASERVER_FIXEDRATEBONDPRICINGREQUEST_H

#include <map>
#include <string>
#include <vector>

#include <ql/qldefines.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/pricingengines/bond/discountingbondengine.hpp>
#include <ql/indexes/ibor/eonia.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/time/imm.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <ql/time/daycounters/actualactual.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>

#include "flatbuffers/grpc.h"

#include "quantra_request.h"

#include "price_fixed_rate_bond_request_generated.h"
#include "fixed_rate_bond_response_generated.h"
#include "common_parser.h"
#include "fixed_rate_bond_parser.h"
#include "term_structure_parser.h"

class FixedRateBondPricingRequest : QuantraRequest<quantra::PriceFixedRateBondRequest,
                                                   quantra::PriceFixedRateBondResponse>
{
public:
    flatbuffers::Offset<quantra::PriceFixedRateBondResponse> request(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const quantra::PriceFixedRateBondRequest *request) const;
};

#endif //QUANTRASERVER_FIXEDRATEBONDPRICINGREQUEST_H