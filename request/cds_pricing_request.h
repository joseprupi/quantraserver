#ifndef QUANTRASERVER_CDS_PRICING_REQUEST_H
#define QUANTRASERVER_CDS_PRICING_REQUEST_H

#include <map>
#include <string>
#include <vector>

#include <ql/qldefines.hpp>
#include <ql/instruments/creditdefaultswap.hpp>
#include <ql/pricingengines/credit/midpointcdsengine.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/termstructures/credit/defaultprobabilityhelpers.hpp>

#include "flatbuffers/grpc.h"

#include "quantra_request.h"

#include "price_cds_request_generated.h"
#include "cds_response_generated.h"
#include "common_parser.h"
#include "cds_parser.h"
#include "term_structure_parser.h"

class CDSPricingRequest : QuantraRequest<quantra::PriceCDSRequest,
                                          quantra::PriceCDSResponse>
{
public:
    flatbuffers::Offset<quantra::PriceCDSResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::PriceCDSRequest *request) const;
};

#endif // QUANTRASERVER_CDS_PRICING_REQUEST_H
