#ifndef QUANTRASERVER_CAP_FLOOR_PRICING_REQUEST_H
#define QUANTRASERVER_CAP_FLOOR_PRICING_REQUEST_H

#include <map>
#include <string>
#include <vector>

#include <ql/qldefines.hpp>
#include <ql/instruments/capfloor.hpp>
#include <ql/pricingengines/capfloor/blackcapfloorengine.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/termstructures/volatility/optionlet/optionletvolatilitystructure.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/time/calendars/target.hpp>

#include "flatbuffers/grpc.h"

#include "quantra_request.h"

#include "price_cap_floor_request_generated.h"
#include "cap_floor_response_generated.h"
#include "common_parser.h"
#include "cap_floor_parser.h"
#include "volatility_parser.h"
#include "term_structure_parser.h"

class CapFloorPricingRequest : QuantraRequest<quantra::PriceCapFloorRequest,
                                               quantra::PriceCapFloorResponse>
{
public:
    flatbuffers::Offset<quantra::PriceCapFloorResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::PriceCapFloorRequest *request) const;
};

#endif // QUANTRASERVER_CAP_FLOOR_PRICING_REQUEST_H
