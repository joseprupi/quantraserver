#include "commodity_observation.h"

#include "date_convert.h"
#include "error.h"

namespace quantra { namespace commodities {

Observation ObserveFutures(
    const FuturesContract& contract,
    std::size_t monthIndex,
    const QuantLib::Date& day,
    const QuantLib::Date& asOf) {
    const auto& month = contract.months().at(monthIndex);
    Observation obs;
    obs.date = day;
    obs.contractMonth = month.month;
    if (day <= asOf) {
        const auto fixing = contract.fixing(month.month, day);
        if (fixing) {
            obs.price = *fixing;
            obs.source = PriceSource::Fixing;
            return obs;
        }
        if (day < asOf) {
            CommodityReadData data;
            data.reason = "MissingFixing";
            data.date = DateToIso(day);
            data.month = month.month;
            data.contractId = contract.id();
            throw CommodityReadError(
                "commodity contract '" + contract.id() + "': no fixing of " + month.month +
                    " on pricing day " + DateToIso(day) + " (before the valuation date)",
                data);
        }
    }
    if (!month.price) {
        CommodityReadData data;
        data.reason = "MonthWithoutPrice";
        data.date = DateToIso(day);
        data.month = month.month;
        data.contractId = contract.id();
        throw CommodityReadError(
            "commodity contract '" + contract.id() + "': month " + month.month +
                " has no price and is read on " + DateToIso(day),
            data);
    }
    obs.price = *month.price;
    obs.source = PriceSource::Curve;
    return obs;
}

SpotObservation ObserveSpot(
    const SpotPrice& spot, const QuantLib::Date& day, const QuantLib::Date& asOf) {
    SpotObservation out;
    Observation& obs = out.observation;
    obs.date = day;
    if (day <= asOf) {
        const auto fixing = spot.fixing(day);
        if (fixing) {
            obs.price = *fixing;
            obs.source = PriceSource::Fixing;
            return out;
        }
        if (day < asOf) {
            CommodityReadData data;
            data.reason = "MissingFixing";
            data.date = DateToIso(day);
            data.spotId = spot.id();
            throw CommodityReadError(
                "commodity spot price '" + spot.id() + "': no fixing on pricing day " +
                    DateToIso(day) + " (before the valuation date)",
                data);
        }
    }
    out.weights = spot.weights(day);
    obs.price = spot.weightedPrice(out.weights);
    obs.source = PriceSource::Curve;
    return out;
}

}} // namespace quantra::commodities
