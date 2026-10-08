#ifndef QUANTRA_PRODUCT_ENDPOINT_H
#define QUANTRA_PRODUCT_ENDPOINT_H

#include "calendar_override_parser.h"
#include "calendar_overrides.h"
#include "eval_date_guard.h"
#include "fixings_guard.h"
#include "pricing_context.h"
#include "pricing_registry.h"
#include "quantra_request.h"

#include "flatbuffers/grpc.h"

#include <exception>
#include <memory>
#include <string>
#include <type_traits>

namespace quantra {

namespace detail {

/// Detects whether a FlatBuffers request table exposes a `.pricing()`
/// accessor (i.e. carries a Pricing block of market data). Utility
/// endpoints such as the calendar lookups have no pricing block; this
/// trait lets ProductEndpoint skip registry/context construction for them.
template <class T, class = void> struct has_pricing : std::false_type {};

template <class T>
struct has_pricing<T, std::void_t<decltype(std::declval<const T&>().pricing())>> : std::true_type {
};

/// Detects whether a FlatBuffers request table carries `calendar_overrides()`
/// directly (the calendar utility requests, which have no Pricing block).
template <class T, class = void> struct has_calendar_overrides : std::false_type {};

template <class T>
struct has_calendar_overrides<
    T,
    std::void_t<decltype(std::declval<const T&>().calendar_overrides())>> : std::true_type {};

/// Detects whether a Mapper exposes an
/// `onRegistryBuildError(Inputs&, const std::string&) const` hook. This is the
/// single opt-in policy that distinguishes list/query endpoints (whose
/// response is a list of per-item outcomes) from single-result endpoints.
///
/// When present, ProductEndpoint wraps registry/context construction in a
/// try/catch: on failure it calls the hook, which folds the build-error
/// message into each query's per-item error field, and then lets the evaluator
/// run its normal per-item path (every item ends up carrying the error). The
/// response is a normal HTTP 200 list of per-item errors rather than a
/// transport-level error. When the hook is absent, the registry is built
/// without a try/catch, so a build failure propagates as a transport error —
/// the behaviour every single-result product relies on.
template <class Mapper, class Inputs, class = void>
struct has_build_error_hook : std::false_type {};

template <class Mapper, class Inputs>
struct has_build_error_hook<
    Mapper,
    Inputs,
    std::void_t<decltype(std::declval<const Mapper&>().onRegistryBuildError(
        std::declval<Inputs&>(), std::declval<const std::string&>()))>> : std::true_type {};

} // namespace detail

/**
 * ProductEndpoint - generic implementation of the QuantraRequest::request()
 * seam, shared by every product handler. Mapper and Evaluator are
 * default-constructed members; full template instantiation happens at the pilot
 * product.
 *
 * The glue is identical for every pricing product:
 *
 *   EvalDateGuard guard;                      // handler owns global state
 *   FixingsGuard fixingsGuard;                // ... incl. the index fixing store
 *   CalendarOverridesGuard calendarGuard;     // ... incl. holiday overrides
 *   mapper.toInputs(req);                     // FlatBuffers -> domain
 *   PricingRegistryBuilder{}.build(pricing);  // market data
 *   makeContext(pricing, reg);                // ambient: asOf/settlement/options
 *   evaluator.evaluate(inputs, reg, ctx);     // QuantLib only (no FB / no gRPC)
 *   mapper.toResponse(builder, result);       // domain -> FlatBuffers
 *
 * For utility endpoints whose request carries no Pricing block (e.g. the
 * calendar lookups), registry/context construction is elided; the evaluator
 * still receives default-constructed `reg`/`ctx` to keep the signature
 * uniform across products.
 *
 * List/query endpoints (e.g. inflation-curve bootstrap, vol-surface sampling)
 * opt into a single extra policy by exposing a mapper `onRegistryBuildError`
 * hook (see detail::has_build_error_hook): a registry-build failure is folded
 * into each query's per-item error and reported as a per-item outcome at HTTP
 * 200, rather than surfacing as a transport error. A malformed top-level
 * request (mapper.toInputs throwing) always propagates as a transport error,
 * for every product.
 */
template <class Req, class Resp, class Mapper, class Evaluator>
class ProductEndpoint : public QuantraRequest<Req, Resp> {
public:
    flatbuffers::Offset<Resp> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const Req* req,
        const RequestBudget& budget = RequestBudget::unlimited()) const override {
        EvalDateGuard guard;
        // Index fixings are process-global QuantLib state too: start every
        // request from an empty fixing store so the only fixings visible are
        // the ones this request supplies (and leave it empty on exit).
        FixingsGuard fixingsGuard;
        // Holiday overrides are process-global QuantLib state: reset on entry
        // and exit, and apply before the mapper runs (mappers already build
        // schedules and advance dates). A malformed overrides field is a
        // transport-level error on every endpoint.
        CalendarOverridesGuard calendarGuard;
        if constexpr (detail::has_pricing<Req>::value) {
            if (req->pricing() != nullptr) {
                applyRequestCalendarOverrides(
                    req->pricing()->calendar_overrides(), "pricing.calendar_overrides");
            }
        } else if constexpr (detail::has_calendar_overrides<Req>::value) {
            applyRequestCalendarOverrides(req->calendar_overrides(), "calendar_overrides");
        }
        auto inputs = mapper_.toInputs(req);
        // Bail out before touching market data if the caller has already timed
        // out (curve bootstrapping is the single most expensive step).
        budget.check();
        PricingRegistry reg;
        PricingContext ctx;
        if constexpr (detail::has_pricing<Req>::value) {
            if constexpr (detail::has_build_error_hook<Mapper, decltype(inputs)>::value) {
                // List/query endpoint: a registry-build failure becomes a
                // per-item error on every query (the evaluator then emits them),
                // so the response stays a per-item list at HTTP 200.
                try {
                    reg = PricingRegistryBuilder{}.build(req->pricing(), budget);
                    ctx = makeContext(req->pricing(), reg);
                } catch (const std::exception& e) {
                    mapper_.onRegistryBuildError(inputs, e.what());
                }
            } else {
                // Single-result endpoint: a build failure propagates as a
                // transport-level error.
                reg = PricingRegistryBuilder{}.build(req->pricing(), budget);
                ctx = makeContext(req->pricing(), reg);
            }
        }
        // Re-check after the registry build, then hand the budget to the
        // evaluator so heavy per-trade loops can honor it too.
        budget.check();
        ctx.budget = budget;
        auto result = evaluator_.evaluate(inputs, reg, ctx);
        return mapper_.toResponse(*builder, result);
    }

private:
    Mapper mapper_;
    Evaluator evaluator_;
};

} // namespace quantra

#endif // QUANTRA_PRODUCT_ENDPOINT_H
