#include "commodity_swap.h"

#include "commodity_pricing_days.h"
#include "date_convert.h"
#include "error.h"

#include <cmath>
#include <map>
#include <tuple>

namespace quantra { namespace commodities {

namespace {

bool isFixed(const CommoditySwapLeg& leg) {
    return std::holds_alternative<FixedPrice>(leg.price);
}

const PricingCalendar& floatingCalendar(const CommoditySwapLeg& leg) {
    if (const auto* f = std::get_if<FuturesFloatingPrice>(&leg.price)) {
        return f->contract->pricingCalendar();
    }
    return std::get<SpotFloatingPrice>(leg.price).spot->pricingCalendar();
}

double floatingSpread(const CommoditySwapLeg& leg) {
    if (const auto* f = std::get_if<FuturesFloatingPrice>(&leg.price)) {
        return f->spread;
    }
    return std::get<SpotFloatingPrice>(leg.price).spread;
}

/// `leg1.periods[2]`: a period of the leg, generated or explicit.
std::string periodPath(const std::string& leg, std::size_t i) {
    return leg + ".periods[" + std::to_string(i) + "]";
}

std::string periodDates(const CommodityPeriod& period) {
    return DateToIso(period.start) + " to " + DateToIso(period.end);
}

/// One value per period from a single value (`path.singleName`) or a
/// per-period list (`path.listName`): exactly one of the two forms, the list
/// as long as the periods, every value accepted by `valid` (otherwise an
/// invalid argument naming the value and `requirement`).
template <typename Valid>
std::vector<double> perPeriod(
    const std::optional<double>& single,
    const std::vector<double>& list,
    std::size_t periods,
    const std::string& path,
    const std::string& singleName,
    const std::string& listName,
    Valid valid,
    const std::string& requirement) {
    if (single.has_value() == !list.empty()) {
        QUANTRA_INVALID_ARGUMENT(
            path + ": give exactly one of " + singleName + " (every period) and " + listName +
            " (one per period)");
    }
    if (single) {
        if (!valid(*single)) {
            QUANTRA_INVALID_ARGUMENT(path + "." + singleName + " " + requirement);
        }
        return std::vector<double>(periods, *single);
    }
    if (list.size() != periods) {
        QUANTRA_INVALID_ARGUMENT(
            path + "." + listName + " has " + std::to_string(list.size()) + " entries for " +
            std::to_string(periods) + " periods");
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (!valid(list[i])) {
            QUANTRA_INVALID_ARGUMENT(
                path + "." + listName + "[" + std::to_string(i) + "] " + requirement);
        }
    }
    return list;
}

/// Checks of one leg on its own: periods, payment, discounting and the
/// floating price's terms.
void validateLeg(const CommoditySwapLeg& leg, const std::string& name) {
    ValidatePeriods(leg.periods, name + ".periods.periods");
    if (leg.paymentCalendar.empty()) {
        QUANTRA_INVALID_ARGUMENT(name + ".payment_calendar is required");
    }
    if (leg.paymentLag < 0) {
        QUANTRA_INVALID_ARGUMENT(
            name + ".payment_lag must be >= 0, got " + std::to_string(leg.paymentLag));
    }
    if (leg.discountCurve.empty()) {
        QUANTRA_INVALID_ARGUMENT(name + ".discounting_curve: no discount curve given");
    }
    if (const auto* f = std::get_if<FuturesFloatingPrice>(&leg.price)) {
        if (!f->contract) {
            QUANTRA_INVALID_ARGUMENT(name + ".price.contract_id: no futures contract given");
        }
        ValidateNearbyAndRoll(f->nearby, f->rollDaysBeforeExpiry, name + ".price");
        if (!std::isfinite(f->spread)) {
            QUANTRA_INVALID_ARGUMENT(name + ".price.spread must be finite");
        }
    } else if (const auto* s = std::get_if<SpotFloatingPrice>(&leg.price)) {
        if (!s->spot) {
            QUANTRA_INVALID_ARGUMENT(name + ".price.spot_id: no spot price given");
        }
        if (!std::isfinite(s->spread)) {
            QUANTRA_INVALID_ARGUMENT(name + ".price.spread must be finite");
        }
    }
}

/// A validated leg with its name and its per-period quantities and fixed
/// prices (fixed legs only).
struct LegInputs {
    const CommoditySwapLeg* leg = nullptr;
    std::string name;
    std::vector<double> quantities;
    std::vector<double> fixedPrices;
};

LegInputs legInputs(const CommoditySwapLeg& leg, const std::string& name) {
    LegInputs out;
    out.leg = &leg;
    out.name = name;
    const std::size_t n = leg.periods.size();
    out.quantities = perPeriod(
        leg.quantity.value, leg.quantity.values, n, name + ".quantity", "value", "values",
        [](double q) { return std::isfinite(q) && q >= 0.0; }, "must be finite and >= 0");
    if (const auto* fixed = std::get_if<FixedPrice>(&leg.price)) {
        out.fixedPrices = perPeriod(
            fixed->price, fixed->prices, n, name + ".price", "price", "prices",
            [](double p) { return std::isfinite(p); }, "must be finite");
    }
    return out;
}

/// Futures deltas first (by contract id and month order), then spot deltas
/// (by spot id and point order).
using DeltaKey = std::tuple<CommodityPriceDelta::Kind, std::string, std::size_t>;
using DeltaMap = std::map<DeltaKey, CommodityPriceDelta>;

/// Reads the price of a floating leg on one pricing day and books the NPV
/// change per unit change of every price read from today's market data.
class FloatingReader {
public:
    explicit FloatingReader(const CommoditySwapLeg& leg)
        : futures_(std::get_if<FuturesFloatingPrice>(&leg.price)),
          spot_(std::get_if<SpotFloatingPrice>(&leg.price)) {
        if (futures_) {
            rollDates_ = futures_->contract->rollDates(futures_->rollDaysBeforeExpiry);
        }
    }

    /// The observation on `day`; `perDay` is the NPV change per unit change
    /// of that day's price.
    Observation read(
        const QuantLib::Date& day,
        const QuantLib::Date& asOf,
        double perDay,
        DeltaMap& deltas) const {
        if (futures_) {
            const auto& contract = *futures_->contract;
            const std::size_t m = contract.selectMonth(day, futures_->nearby, rollDates_);
            Observation obs = ObserveFutures(contract, m, day, asOf);
            if (obs.source == PriceSource::Curve) {
                auto& d = deltas[DeltaKey{CommodityPriceDelta::Kind::Futures, contract.id(), m}];
                d.kind = CommodityPriceDelta::Kind::Futures;
                d.contractId = contract.id();
                d.month = contract.months()[m].month;
                d.delta += perDay;
            }
            return obs;
        }
        const auto& spot = *spot_->spot;
        SpotObservation spotObs = ObserveSpot(spot, day, asOf);
        for (const auto& w : spotObs.weights) {
            auto& d = deltas[DeltaKey{CommodityPriceDelta::Kind::Spot, spot.id(), w.point}];
            d.kind = CommodityPriceDelta::Kind::Spot;
            d.spotId = spot.id();
            d.date = spot.points()[w.point].date;
            d.delta += perDay * w.weight;
        }
        return std::move(spotObs.observation);
    }

private:
    const FuturesFloatingPrice* futures_;
    const SpotFloatingPrice* spot_;
    std::vector<QuantLib::Date> rollDates_;
};

/// Payment date of a period: the payment calendar's advance of the period's
/// last day by the payment lag, adjusted by the convention.
QuantLib::Date paymentDate(const CommoditySwapLeg& leg, const CommodityPeriod& period) {
    return leg.paymentCalendar.advance(
        period.end, leg.paymentLag, QuantLib::Days, leg.paymentConvention);
}

/// The pricing days a period of `leg` counts: a floating leg's own; for a
/// fixed leg with quantity per pricing day, those of the floating leg's
/// period with the same dates; none for a fixed leg with quantity per period.
std::vector<QuantLib::Date> countedPricingDays(
    const LegInputs& leg, std::size_t i, const LegInputs& other) {
    const auto& period = leg.leg->periods[i];
    if (!isFixed(*leg.leg)) {
        return PeriodPricingDays(floatingCalendar(*leg.leg), period, periodPath(leg.name, i));
    }
    if (leg.leg->quantity.basis != QuantityBasis::PerPricingDay) {
        return {};
    }
    const auto& otherPeriods = other.leg->periods;
    for (std::size_t j = 0; j < otherPeriods.size(); ++j) {
        if (otherPeriods[j].start == period.start && otherPeriods[j].end == period.end) {
            return PeriodPricingDays(
                floatingCalendar(*other.leg), otherPeriods[j], periodPath(other.name, j));
        }
    }
    QUANTRA_INVALID_ARGUMENT(
        periodPath(leg.name, i) + " (" + periodDates(period) +
        "): a fixed leg quantity per pricing day needs a floating leg period with the same dates");
    return {}; // unreachable
}

/// Fills a floating flow's observations and averages from its pricing days.
void averageFloating(
    const FloatingReader& reader,
    const std::vector<QuantLib::Date>& days,
    const QuantLib::Date& asOf,
    double perDay,
    CommodityFlow& flow,
    DeltaMap& deltas) {
    double sumAll = 0.0, sumFixing = 0.0, sumCurve = 0.0;
    int curveDays = 0;
    for (const auto& day : days) {
        Observation obs = reader.read(day, asOf, perDay, deltas);
        sumAll += obs.price;
        if (obs.source == PriceSource::Fixing) {
            sumFixing += obs.price;
            ++flow.realisedDays;
        } else {
            sumCurve += obs.price;
            ++curveDays;
        }
        flow.observations.push_back(std::move(obs));
    }
    flow.average = sumAll / days.size();
    if (flow.realisedDays > 0) {
        flow.realisedAverage = sumFixing / flow.realisedDays;
    }
    if (curveDays > 0) {
        flow.expectedAverage = sumCurve / curveDays;
    }
}

[[noreturn]] void notFinite(const std::string& what) {
    throw QuantraInvalidArgument(what + " is not finite (the numbers overflowed)");
}

struct LegOutcome {
    CommodityLegResult result;
    /// Sum over live flows of sign * quantity * discount: the NPV change per
    /// unit change of every period's price (fixed price or spread).
    double annuity = 0.0;
};

LegOutcome priceLeg(
    const LegInputs& inputs, const LegInputs& other, const QuantLib::Date& asOf, DeltaMap& deltas) {
    const CommoditySwapLeg& leg = *inputs.leg;
    const bool fixed = isFixed(leg);
    const double sign = leg.payReceive == PayReceive::Pay ? -1.0 : 1.0;
    std::optional<FloatingReader> reader;
    if (!fixed) {
        reader.emplace(leg);
    }

    LegOutcome out;
    for (std::size_t i = 0; i < leg.periods.size(); ++i) {
        const auto& period = leg.periods[i];
        CommodityFlow flow;
        flow.start = period.start;
        flow.end = period.end;
        flow.paymentDate = paymentDate(leg, period);
        // Paid on or before the valuation date: no flow, nothing counted or read.
        if (flow.paymentDate <= asOf) {
            continue;
        }

        const auto days = countedPricingDays(inputs, i, other);
        const int dayCount = static_cast<int>(days.size());
        flow.pricingDays = dayCount;
        flow.quantity = inputs.quantities[i];
        if (leg.quantity.basis == QuantityBasis::PerPricingDay) {
            flow.quantity *= dayCount;
        }
        flow.discount = leg.discountCurve->discount(flow.paymentDate);

        if (fixed) {
            flow.price = inputs.fixedPrices[i];
            flow.amount = sign * flow.quantity * inputs.fixedPrices[i];
        } else {
            const double spread = floatingSpread(leg);
            // NPV change per unit change of one day's price.
            const double perDay = sign * flow.quantity * flow.discount / days.size();
            averageFloating(*reader, days, asOf, perDay, flow, deltas);
            flow.spread = spread;
            flow.amount = sign * flow.quantity * (*flow.average + spread);
        }
        flow.presentValue = flow.amount * flow.discount;
        if (!std::isfinite(flow.amount) || !std::isfinite(flow.presentValue)) {
            notFinite(periodPath(inputs.name, i) + " flow amount or present value");
        }
        out.result.npv += flow.presentValue;
        out.annuity += sign * flow.quantity * flow.discount;
        out.result.flows.push_back(std::move(flow));
    }
    return out;
}

} // namespace

CommoditySwapResult PriceCommoditySwap(
    const CommoditySwapLeg& leg1, const CommoditySwapLeg& leg2, const QuantLib::Date& asOf) {
    if (asOf == QuantLib::Date()) {
        QUANTRA_INVALID_ARGUMENT("valuation date is required");
    }
    validateLeg(leg1, "leg1");
    validateLeg(leg2, "leg2");
    if (leg1.payReceive == leg2.payReceive) {
        QUANTRA_INVALID_ARGUMENT(
            "leg1.pay_receive and leg2.pay_receive must be opposite (one Pay, one Receive)");
    }
    const bool fixed1 = isFixed(leg1), fixed2 = isFixed(leg2);
    const std::array<LegInputs, 2> inputs = {legInputs(leg1, "leg1"), legInputs(leg2, "leg2")};
    if (fixed1 && fixed2) {
        for (const auto& in : inputs) {
            if (in.leg->quantity.basis == QuantityBasis::PerPricingDay) {
                QUANTRA_INVALID_ARGUMENT(
                    in.name +
                    ".quantity.basis: a fixed leg quantity per pricing day needs a floating leg "
                    "to count the pricing days");
            }
        }
    }

    CommoditySwapResult result;
    DeltaMap deltas;
    std::array<double, 2> annuity = {0.0, 0.0};
    for (std::size_t k = 0; k < 2; ++k) {
        auto outcome = priceLeg(inputs[k], inputs[1 - k], asOf, deltas);
        result.legs[k] = std::move(outcome.result);
        annuity[k] = outcome.annuity;
    }
    result.npv = result.legs[0].npv + result.legs[1].npv;
    // A leg NPV that is not finite leaves the total not finite.
    if (!std::isfinite(result.npv)) {
        notFinite("npv");
    }

    if (fixed1 != fixed2) {
        // npv(K) = floating NPV + K * fixed annuity.
        const std::size_t f = fixed1 ? 0 : 1;
        if (annuity[f] != 0.0) {
            result.fairFixedPrice = -result.legs[1 - f].npv / annuity[f];
            if (!std::isfinite(*result.fairFixedPrice)) {
                notFinite("fair_fixed_price");
            }
        }
    } else if (!fixed1 && !fixed2) {
        // npv(s) = npv + (s - current spread) * leg 2 annuity.
        if (annuity[1] != 0.0) {
            result.fairSpread = floatingSpread(leg2) - result.npv / annuity[1];
            if (!std::isfinite(*result.fairSpread)) {
                notFinite("fair_spread");
            }
        }
    }

    for (auto& entry : deltas) {
        if (!std::isfinite(entry.second.delta)) {
            notFinite("a price delta");
        }
        result.deltas.push_back(entry.second);
    }
    return result;
}

std::string QualifySwapMessage(const std::string& swap, const std::string& message) {
    const bool legPath = message.rfind("leg1", 0) == 0 || message.rfind("leg2", 0) == 0;
    return swap + (legPath ? "." : ": ") + message;
}

}} // namespace quantra::commodities
