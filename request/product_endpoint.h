#ifndef QUANTRA_PRODUCT_ENDPOINT_H
#define QUANTRA_PRODUCT_ENDPOINT_H

#include <memory>

#include "flatbuffers/grpc.h"

#include "quantra_request.h"
#include "pricing_registry.h"
#include "pricing_context.h"
#include "eval_date_guard.h"

namespace quantra {

/**
 * ProductEndpoint - generic implementation of the QuantraRequest::request()
 * seam, shared by every product handler. Mapper and Pricer are
 * default-constructed members; full template instantiation happens at the pilot
 * product.
 *
 * The glue is identical for every product:
 *
 *   EvalDateGuard guard;                      // handler owns global state
 *   mapper.toInputs(req);                     // FlatBuffers -> domain
 *   PricingRegistryBuilder{}.build(pricing);  // market data
 *   makeContext(pricing, reg);                // ambient: asOf/settlement/options
 *   pricer.price(inputs, reg, ctx);           // QuantLib only (no FB / no gRPC)
 *   mapper.toResponse(builder, result);       // domain -> FlatBuffers
 */
template <class Req, class Resp, class Mapper, class Pricer>
class ProductEndpoint : public QuantraRequest<Req, Resp> {
public:
    flatbuffers::Offset<Resp> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const Req* req) const override
    {
        EvalDateGuard guard;
        auto inputs = mapper_.toInputs(req);
        PricingRegistry reg = PricingRegistryBuilder{}.build(req->pricing());
        PricingContext ctx = makeContext(req->pricing(), reg);
        auto result = pricer_.price(inputs, reg, ctx);
        return mapper_.toResponse(*builder, result);
    }

private:
    Mapper mapper_;
    Pricer pricer_;
};

} // namespace quantra

#endif // QUANTRA_PRODUCT_ENDPOINT_H
