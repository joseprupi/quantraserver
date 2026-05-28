#ifndef QUANTRA_PRODUCT_ENDPOINT_H
#define QUANTRA_PRODUCT_ENDPOINT_H

#include <memory>
#include <type_traits>

#include "flatbuffers/grpc.h"

#include "quantra_request.h"
#include "pricing_registry.h"
#include "pricing_context.h"
#include "eval_date_guard.h"

namespace quantra {

namespace detail {

/// Detects whether a FlatBuffers request table exposes a `.pricing()`
/// accessor (i.e. carries a Pricing block of market data). Utility
/// endpoints such as the calendar lookups have no pricing block; this
/// trait lets ProductEndpoint skip registry/context construction for them.
template <class T, class = void>
struct has_pricing : std::false_type {};

template <class T>
struct has_pricing<T, std::void_t<decltype(std::declval<const T&>().pricing())>>
    : std::true_type {};

} // namespace detail

/**
 * ProductEndpoint - generic implementation of the QuantraRequest::request()
 * seam, shared by every product handler. Mapper and Pricer are
 * default-constructed members; full template instantiation happens at the pilot
 * product.
 *
 * The glue is identical for every pricing product:
 *
 *   EvalDateGuard guard;                      // handler owns global state
 *   mapper.toInputs(req);                     // FlatBuffers -> domain
 *   PricingRegistryBuilder{}.build(pricing);  // market data
 *   makeContext(pricing, reg);                // ambient: asOf/settlement/options
 *   pricer.price(inputs, reg, ctx);           // QuantLib only (no FB / no gRPC)
 *   mapper.toResponse(builder, result);       // domain -> FlatBuffers
 *
 * For utility endpoints whose request carries no Pricing block (e.g. the
 * calendar lookups), registry/context construction is elided; the pricer
 * still receives default-constructed `reg`/`ctx` to keep the signature
 * uniform across products.
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
        PricingRegistry reg;
        PricingContext ctx;
        if constexpr (detail::has_pricing<Req>::value) {
            reg = PricingRegistryBuilder{}.build(req->pricing());
            ctx = makeContext(req->pricing(), reg);
        }
        auto result = pricer_.price(inputs, reg, ctx);
        return mapper_.toResponse(*builder, result);
    }

private:
    Mapper mapper_;
    Pricer pricer_;
};

} // namespace quantra

#endif // QUANTRA_PRODUCT_ENDPOINT_H
