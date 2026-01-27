#ifndef QUANTRASERVER_VANILLA_SWAP_PRICING_REQUEST_H
#define QUANTRASERVER_VANILLA_SWAP_PRICING_REQUEST_H

#include <map>
#include <string>
#include <vector>

#include <ql/qldefines.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/cashflows/iborcoupon.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <ql/time/daycounters/actualactual.hpp>

#include "flatbuffers/grpc.h"

#include "quantra_request.h"

#include "price_vanilla_swap_request_generated.h"
#include "vanilla_swap_response_generated.h"
#include "common_parser.h"
#include "vanilla_swap_parser.h"
#include "term_structure_parser.h"

class VanillaSwapPricingRequest : QuantraRequest<quantra::PriceVanillaSwapRequest,
                                                  quantra::PriceVanillaSwapResponse>
{
public:
    flatbuffers::Offset<quantra::PriceVanillaSwapResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::PriceVanillaSwapRequest *request) const;
};

#endif // QUANTRASERVER_VANILLA_SWAP_PRICING_REQUEST_H