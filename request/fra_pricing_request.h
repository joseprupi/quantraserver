#ifndef QUANTRASERVER_FRA_PRICING_REQUEST_H
#define QUANTRASERVER_FRA_PRICING_REQUEST_H

#include <map>
#include <string>
#include <vector>

#include <ql/qldefines.hpp>
#include <ql/instruments/forwardrateagreement.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/time/calendars/target.hpp>

#include "flatbuffers/grpc.h"

#include "quantra_request.h"

#include "price_fra_request_generated.h"
#include "fra_response_generated.h"
#include "common_parser.h"
#include "fra_parser.h"
#include "term_structure_parser.h"

class FRAPricingRequest : QuantraRequest<quantra::PriceFRARequest,
                                          quantra::PriceFRAResponse>
{
public:
    flatbuffers::Offset<quantra::PriceFRAResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::PriceFRARequest *request) const;
};

#endif // QUANTRASERVER_FRA_PRICING_REQUEST_H
