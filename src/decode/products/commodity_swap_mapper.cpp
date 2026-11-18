#include "commodity_swap_mapper.h"

#include "commodity_periods.h"
#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"
#include "request_validation.h"

#include <string>
#include <vector>

namespace quantra {

namespace {

namespace cm = quantra::commodities;

} // namespace

/// Exactly one form: generated (start_date + end_date + frequency, no
/// periods) or explicit (periods only). The periods' own rules (order,
/// overlaps, a supported frequency) belong to the library.
std::vector<commodities::CommodityPeriod> DecodeCommodityPeriods(
    const quantra::CommodityPeriods* periods, const std::string& field) {
    namespace cm = quantra::commodities;
    if (!periods) {
        QuantraInvalidArgumentAt(field, field + " is required");
    }
    const bool hasStart = periods->start_date() != nullptr;
    const bool hasEnd = periods->end_date() != nullptr;
    const bool hasFrequency = periods->frequency().has_value();
    const bool hasList = periods->periods() != nullptr;
    const bool anyGenerated = hasStart || hasEnd || hasFrequency;
    if (hasList && anyGenerated) {
        QuantraInvalidArgumentAt(
            field, field +
                       ": give either start_date, end_date and frequency (generated) or periods "
                       "(explicit), not both");
    }
    if (!hasList && !(hasStart && hasEnd && hasFrequency)) {
        if (!anyGenerated) {
            QuantraInvalidArgumentAt(
                field,
                field + ": give either start_date, end_date and frequency (generated) or periods "
                        "(explicit)");
        }
        const std::string missing = !hasStart ? "start_date" : !hasEnd ? "end_date" : "frequency";
        QuantraInvalidArgumentAt(
            field + "." + missing,
            field + "." + missing +
                " is required: generated periods need start_date, end_date and frequency");
    }

    if (!hasList) {
        const QuantLib::Date start = requireIsoDate(periods->start_date(), field + ".start_date");
        const QuantLib::Date end = requireIsoDate(periods->end_date(), field + ".end_date");
        return cm::GeneratePeriods(start, end, FrequencyToQL(periods->frequency().value()), field);
    }
    std::vector<cm::CommodityPeriod> out;
    const auto* list = periods->periods();
    out.reserve(list->size());
    for (flatbuffers::uoffset_t i = 0; i < list->size(); ++i) {
        const auto* p = list->Get(i);
        const std::string at = field + ".periods[" + std::to_string(i) + "]";
        if (!p) {
            QuantraInvalidArgumentAt(at, at + " is null");
        }
        const QuantLib::Date start = requireIsoDate(p->start_date(), at + ".start_date");
        const QuantLib::Date end = requireIsoDate(p->end_date(), at + ".end_date");
        out.push_back({start, end});
    }
    return out;
}

namespace {

namespace cm = quantra::commodities;

/// A per-period list given on the wire. Present with no entry is refused
/// here: the library cannot tell it from an absent list.
std::vector<double> decodeList(const flatbuffers::Vector<double>* list, const std::string& field) {
    if (list == nullptr) {
        return {};
    }
    if (list->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(field + " must not be empty when given");
    }
    return std::vector<double>(list->begin(), list->end());
}

cm::CommodityQuantity decodeQuantity(
    const quantra::CommodityQuantity* quantity, const std::string& field) {
    if (!quantity) {
        QUANTRA_INVALID_ARGUMENT(field + " is required");
    }
    cm::CommodityQuantity out;
    if (quantity->value().has_value()) {
        out.value = quantity->value().value();
    }
    out.values = decodeList(quantity->values(), field + ".values");
    switch (requireEnum(quantity->basis(), field + ".basis")) {
        case quantra::enums::CommodityQuantityBasis_PerPeriod:
            out.basis = cm::QuantityBasis::PerPeriod;
            break;
        case quantra::enums::CommodityQuantityBasis_PerPricingDay:
            out.basis = cm::QuantityBasis::PerPricingDay;
            break;
        default: QUANTRA_INVALID_ARGUMENT(field + ".basis is not a known value");
    }
    return out;
}

cm::FixedPrice decodeFixedPrice(
    const quantra::CommodityFixedPrice* price, const std::string& field) {
    cm::FixedPrice out;
    if (price->price().has_value()) {
        out.price = price->price().value();
    }
    out.prices = decodeList(price->prices(), field + ".prices");
    return out;
}

CommoditySwapLegSpec decodeLeg(const quantra::CommoditySwapLeg* leg, const std::string& name) {
    if (!leg) {
        QUANTRA_INVALID_ARGUMENT(name + " is required");
    }
    CommoditySwapLegSpec spec;
    cm::CommoditySwapLeg& out = spec.leg;
    out.payReceive =
        requireEnum(leg->pay_receive(), name + ".pay_receive") == quantra::enums::PayReceive_Pay
            ? cm::PayReceive::Pay
            : cm::PayReceive::Receive;
    out.periods = DecodeCommodityPeriods(leg->periods(), name + ".periods");
    out.quantity = decodeQuantity(leg->quantity(), name + ".quantity");

    const std::string priceField = name + ".price";
    switch (leg->price_type()) {
        case quantra::CommodityLegPrice_CommodityFixedPrice:
            out.price = decodeFixedPrice(leg->price_as_CommodityFixedPrice(), priceField);
            break;
        case quantra::CommodityLegPrice_FuturesFloatingPrice: {
            const auto* f = leg->price_as_FuturesFloatingPrice();
            cm::FuturesFloatingPrice futures;
            spec.contractId = requireNonEmptyString(f->contract_id(), priceField + ".contract_id");
            futures.nearby = requireInt(f->nearby(), priceField + ".nearby");
            futures.rollDaysBeforeExpiry =
                requireInt(f->roll_days_before_expiry(), priceField + ".roll_days_before_expiry");
            futures.spread = f->spread();
            out.price = futures;
            break;
        }
        case quantra::CommodityLegPrice_SpotFloatingPrice: {
            const auto* s = leg->price_as_SpotFloatingPrice();
            cm::SpotFloatingPrice spot;
            spec.spotId = requireNonEmptyString(s->spot_id(), priceField + ".spot_id");
            spot.spread = s->spread();
            out.price = spot;
            break;
        }
        default:
            QUANTRA_INVALID_ARGUMENT(
                priceField +
                " is required (CommodityFixedPrice, FuturesFloatingPrice or SpotFloatingPrice)");
    }

    out.paymentCalendar =
        CalendarToQL(requireEnum(leg->payment_calendar(), name + ".payment_calendar"));
    out.paymentLag = requireInt(leg->payment_lag(), name + ".payment_lag");
    out.paymentConvention =
        ConventionToQL(requireEnum(leg->payment_convention(), name + ".payment_convention"));
    spec.discountingCurveId =
        requireNonEmptyString(leg->discounting_curve(), name + ".discounting_curve");
    return spec;
}

CommoditySwapTrade extractTrade(const quantra::PriceCommoditySwap* entry, std::size_t index) {
    const std::string base = "swaps[" + std::to_string(index) + "]";
    if (entry == nullptr) {
        QUANTRA_INVALID_ARGUMENT(base + " is null");
    }
    const auto* swap = entry->commodity_swap();
    const std::string path = base + ".commodity_swap";
    if (!swap) {
        QUANTRA_INVALID_ARGUMENT(path + " is required");
    }
    CommoditySwapTrade trade;
    trade.legs[0] = decodeLeg(swap->leg1(), path + ".leg1");
    trade.legs[1] = decodeLeg(swap->leg2(), path + ".leg2");
    return trade;
}

flatbuffers::Offset<flatbuffers::String> isoString(
    flatbuffers::grpc::MessageBuilder& builder, const QuantLib::Date& date) {
    return builder.CreateString(DateToIso(date));
}

flatbuffers::Offset<quantra::CommodityObservation> serializeObservation(
    flatbuffers::grpc::MessageBuilder& builder, const cm::Observation& obs) {
    auto date = isoString(builder, obs.date);
    flatbuffers::Offset<flatbuffers::String> month = 0;
    if (!obs.contractMonth.empty()) {
        month = builder.CreateString(obs.contractMonth);
    }
    quantra::CommodityObservationBuilder ob(builder);
    ob.add_date(date);
    if (!obs.contractMonth.empty()) {
        ob.add_contract_month(month);
    }
    ob.add_price(obs.price);
    ob.add_source(
        obs.source == cm::PriceSource::Fixing ? quantra::enums::CommodityPriceSource_Fixing
                                              : quantra::enums::CommodityPriceSource_Curve);
    return ob.Finish();
}

flatbuffers::Offset<quantra::CommodityFlow> serializeFlow(
    flatbuffers::grpc::MessageBuilder& builder,
    const cm::CommodityFlow& flow,
    bool includeObservations) {
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::CommodityObservation>>>
        observations = 0;
    if (includeObservations) {
        std::vector<flatbuffers::Offset<quantra::CommodityObservation>> rows;
        rows.reserve(flow.observations.size());
        for (const auto& obs : flow.observations) {
            rows.push_back(serializeObservation(builder, obs));
        }
        observations = builder.CreateVector(rows);
    }
    auto start = isoString(builder, flow.start);
    auto end = isoString(builder, flow.end);
    auto payment = isoString(builder, flow.paymentDate);

    quantra::CommodityFlowBuilder fb(builder);
    fb.add_start_date(start);
    fb.add_end_date(end);
    fb.add_payment_date(payment);
    fb.add_pricing_days(flow.pricingDays);
    fb.add_realised_days(flow.realisedDays);
    // Optional by construction: present only when the library sets them.
    if (flow.average) fb.add_average(*flow.average);
    if (flow.realisedAverage) fb.add_realised_average(*flow.realisedAverage);
    if (flow.expectedAverage) fb.add_expected_average(*flow.expectedAverage);
    if (flow.spread) fb.add_spread(*flow.spread);
    if (flow.price) fb.add_price(*flow.price);
    fb.add_quantity(flow.quantity);
    fb.add_amount(flow.amount);
    fb.add_discount(flow.discount);
    fb.add_present_value(flow.presentValue);
    if (includeObservations) {
        fb.add_observations(observations);
    }
    return fb.Finish();
}

flatbuffers::Offset<quantra::CommodityPriceDelta> serializeDelta(
    flatbuffers::grpc::MessageBuilder& builder, const cm::CommodityPriceDelta& delta) {
    const bool futures = delta.kind == cm::CommodityPriceDelta::Kind::Futures;
    flatbuffers::Offset<flatbuffers::String> contractId = 0, month = 0, spotId = 0, date = 0;
    if (futures) {
        contractId = builder.CreateString(delta.contractId);
        month = builder.CreateString(delta.month);
    } else {
        spotId = builder.CreateString(delta.spotId);
        date = isoString(builder, delta.date);
    }
    quantra::CommodityPriceDeltaBuilder db(builder);
    if (futures) {
        db.add_contract_id(contractId);
        db.add_month(month);
    } else {
        db.add_spot_id(spotId);
        db.add_date(date);
    }
    db.add_delta(delta.delta);
    return db.Finish();
}

} // namespace

CommoditySwapInputs CommoditySwapMapper::toInputs(
    const quantra::PriceCommoditySwapRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceCommoditySwapRequest is null");
    }
    const auto* swaps = req->swaps();
    if (swaps == nullptr || swaps->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceCommoditySwapRequest.swaps is required and must be non-empty");
    }
    if (req->include_observations() && !req->include_flows()) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceCommoditySwapRequest.include_observations needs include_flows: observations "
            "are listed per flow");
    }

    CommoditySwapInputs inputs;
    inputs.includeFlows = req->include_flows();
    inputs.includeObservations = req->include_observations();
    inputs.trades.reserve(swaps->size());
    for (flatbuffers::uoffset_t i = 0; i < swaps->size(); ++i) {
        inputs.trades.push_back(extractTrade(swaps->Get(i), i));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceCommoditySwapResponse> CommoditySwapMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder, const CommoditySwapResultSet& result) const {
    std::vector<flatbuffers::Offset<quantra::CommoditySwapResponse>> rows;
    rows.reserve(result.swaps.size());

    for (const auto& swap : result.swaps) {
        std::vector<flatbuffers::Offset<quantra::CommoditySwapLegResult>> legs;
        for (const auto& leg : swap.legs) {
            flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::CommodityFlow>>>
                flows = 0;
            if (result.includeFlows) {
                std::vector<flatbuffers::Offset<quantra::CommodityFlow>> flowRows;
                flowRows.reserve(leg.flows.size());
                for (const auto& flow : leg.flows) {
                    flowRows.push_back(serializeFlow(builder, flow, result.includeObservations));
                }
                flows = builder.CreateVector(flowRows);
            }
            quantra::CommoditySwapLegResultBuilder lb(builder);
            lb.add_npv(leg.npv);
            if (result.includeFlows) {
                lb.add_flows(flows);
            }
            legs.push_back(lb.Finish());
        }
        auto legsVec = builder.CreateVector(legs);

        std::vector<flatbuffers::Offset<quantra::CommodityPriceDelta>> deltas;
        deltas.reserve(swap.deltas.size());
        for (const auto& delta : swap.deltas) {
            deltas.push_back(serializeDelta(builder, delta));
        }
        auto deltasVec = builder.CreateVector(deltas);

        quantra::CommoditySwapResponseBuilder rb(builder);
        rb.add_npv(swap.npv);
        // Optional by construction: fair_fixed_price for a fixed leg against a
        // floating leg, fair_spread for two floating legs, never a 0 stand-in.
        if (swap.fairFixedPrice) {
            rb.add_fair_fixed_price(*swap.fairFixedPrice);
        }
        if (swap.fairSpread) {
            rb.add_fair_spread(*swap.fairSpread);
        }
        rb.add_legs(legsVec);
        rb.add_deltas(deltasVec);
        rows.push_back(rb.Finish());
    }

    auto rowsVec = builder.CreateVector(rows);
    quantra::PriceCommoditySwapResponseBuilder rb(builder);
    rb.add_swaps(rowsVec);
    return rb.Finish();
}

} // namespace quantra
