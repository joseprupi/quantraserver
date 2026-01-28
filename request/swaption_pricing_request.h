#ifndef QUANTRASERVER_SWAPTION_PRICING_REQUEST_H
#define QUANTRASERVER_SWAPTION_PRICING_REQUEST_H

#include <map>
#include <string>
#include <vector>

#include <ql/qldefines.hpp>
#include <ql/instruments/swaption.hpp>
#include <ql/pricingengines/swaption/blackswaptionengine.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/termstructures/volatility/swaption/swaptionconstantvol.hpp>

#include "flatbuffers/grpc.h"

#include "quantra_request.h"

#include "price_swaption_request_generated.h"
#include "swaption_response_generated.h"
#include "common_parser.h"
#include "swaption_parser.h"
#include "term_structure_parser.h"

class SwaptionPricingRequest : QuantraRequest<quantra::PriceSwaptionRequest,
                                               quantra::PriceSwaptionResponse>
{
public:
    flatbuffers::Offset<quantra::PriceSwaptionResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::PriceSwaptionRequest *request) const;
};

#endif // QUANTRASERVER_SWAPTION_PRICING_REQUEST_H