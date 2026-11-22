#include "commodity_swap_evaluator.h"

#include "date_convert.h"
#include "error.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <variant>

namespace quantra {

namespace {

std::string jsonText(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

/// The details of a swap error: reason, swap index, path and the data that
/// apply (only those).
std::string swapDetails(
    const std::string& reason,
    std::size_t swap,
    const std::string& path,
    const commodities::CommodityReadData* data = nullptr) {
    std::string out = "{\"reason\": " + jsonText(reason) + ", \"swap\": " + std::to_string(swap);
    if (!path.empty()) {
        out += ", \"path\": " + jsonText(path);
    }
    if (data != nullptr) {
        if (data->date) {
            out += ", \"date\": " + jsonText(*data->date);
        }
        if (data->month) {
            out += ", \"month\": " + jsonText(*data->month);
        }
        if (data->contractId) {
            out += ", \"contract_id\": " + jsonText(*data->contractId);
        }
        if (data->spotId) {
            out += ", \"spot_id\": " + jsonText(*data->spotId);
        }
    }
    return out + "}";
}

/// The field a library message starts with (`leg2.quantity.values ...`),
/// relative to the swap; empty when it starts with no field.
std::string leadingField(const std::string& message) {
    for (const char* f : {"leg1", "leg2", "common_pricing"}) {
        if (message.rfind(f, 0) == 0) {
            return message.substr(0, message.find_first_of(" :("));
        }
    }
    return "";
}

/// The library leg with its contract / spot price and discount curve
/// resolved from the registry; an unknown id is a 404 naming `path` (the leg,
/// e.g. `swaps[2].commodity_swap.leg1`) and the field, with details.
commodities::CommoditySwapLeg resolveLeg(
    const CommoditySwapLegSpec& spec,
    const PricingRegistry& reg,
    const std::string& path,
    std::size_t swap) {
    commodities::CommoditySwapLeg leg = spec.leg;
    if (auto* futures = std::get_if<commodities::FuturesFloatingPrice>(&leg.price)) {
        const std::string field = path + ".price.contract_id";
        try {
            futures->contract = reg.commodity.contract(spec.contractId, field);
        } catch (const QuantraNotFound& e) {
            throw QuantraNotFound(e.what(), swapDetails("UnknownContract", swap, field));
        }
    } else if (auto* spot = std::get_if<commodities::SpotFloatingPrice>(&leg.price)) {
        const std::string field = path + ".price.spot_id";
        try {
            spot->spot = reg.commodity.spotPrice(spec.spotId, field);
        } catch (const QuantraNotFound& e) {
            throw QuantraNotFound(e.what(), swapDetails("UnknownSpotPrice", swap, field));
        }
    }
    auto curve = reg.rates.curves.find(spec.discountingCurveId);
    if (curve == reg.rates.curves.end() || !curve->second) {
        throw QuantraNotFound(
            path + ".discounting_curve: discounting curve not found: " + spec.discountingCurveId,
            swapDetails("UnknownDiscountingCurve", swap, path + ".discounting_curve"));
    }
    leg.discountCurve = QuantLib::Handle<QuantLib::YieldTermStructure>(*curve->second);
    return leg;
}

bool sameInput(const commodities::QuoteSensitivity& a, const commodities::QuoteSensitivity& b) {
    return a.month == b.month && a.date == b.date && a.averageQuote == b.averageQuote;
}

/// The swap's NPV sensitivity to every input of every built curve it reads:
/// each delta to a built month / point times that output's jacobian row,
/// accumulated by (entry, the input's position in the entry's own inputs).
std::vector<CommodityQuoteDeltaRow> quoteDeltas(
    const commodities::CommoditySwapResult& swap, const CommodityRegistry& commodity) {
    const auto& built = commodity.built;
    std::map<std::pair<std::size_t, std::size_t>, double> acc;
    for (const auto& d : swap.deltas) {
        const bool futures = d.kind == commodities::CommodityPriceDelta::Kind::Futures;
        const std::string& id = futures ? d.contractId : d.spotId;
        const std::string output = futures ? d.month : DateToIso(d.date);
        for (std::size_t b = 0; b < built.size(); ++b) {
            if (built[b].spot == futures || built[b].id != id) {
                continue;
            }
            const auto& outputs = built[b].outputs;
            const auto row = std::find(outputs.begin(), outputs.end(), output);
            if (row == outputs.end()) {
                continue;
            }
            const auto& jac = built[b].jacobian;
            const auto& values = jac.rows[static_cast<std::size_t>(row - outputs.begin())];
            for (std::size_t e = 0; e < jac.inputs.size(); ++e) {
                const auto& input = jac.inputs[e];
                std::size_t curve = b;
                std::size_t ordinal = e;
                if (input.curve) {
                    curve = *input.curve;
                    const auto& own = built[curve].jacobian.inputs;
                    ordinal = own.size();
                    for (std::size_t k = 0; k < own.size(); ++k) {
                        if (!own[k].curve && sameInput(own[k], input)) {
                            ordinal = k;
                        }
                    }
                }
                acc[{curve, ordinal}] += d.delta * values[e];
            }
        }
    }
    std::vector<CommodityQuoteDeltaRow> out;
    for (const auto& [key, delta] : acc) {
        if (!std::isfinite(delta)) {
            QUANTRA_INVALID_ARGUMENT("a quote delta is not finite (the numbers overflowed)");
        }
        CommodityQuoteDeltaRow row;
        row.curveId = built[key.first].id;
        row.input = built[key.first].jacobian.inputs.at(key.second);
        row.delta = delta;
        out.push_back(std::move(row));
    }
    return out;
}

} // namespace

CommoditySwapResultSet CommoditySwapEvaluator::evaluate(
    const CommoditySwapInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    CommoditySwapResultSet result;
    result.includeFlows = inputs.includeFlows;
    result.includeObservations = inputs.includeObservations;
    result.swaps.reserve(inputs.trades.size());
    for (std::size_t i = 0; i < inputs.trades.size(); ++i) {
        ctx.budget.check();
        const auto& trade = inputs.trades[i];
        const std::string path = "swaps[" + std::to_string(i) + "].commodity_swap";
        const auto leg1 = resolveLeg(trade.legs[0], reg, path + ".leg1", i);
        const auto leg2 = resolveLeg(trade.legs[1], reg, path + ".leg2", i);
        try {
            commodities::SwapOptions options;
            options.commonPricing = trade.commonPricing;
            result.swaps.push_back(commodities::PriceCommoditySwap(leg1, leg2, ctx.asOf, options));
        } catch (const commodities::CommodityReadError& e) {
            // A refusal about the data read: its reason and data as details.
            const auto& data = e.data();
            throw QuantraInvalidArgument(
                commodities::QualifySwapMessage(path, e.what()),
                swapDetails(
                    data.reason, i, data.path.empty() ? path : path + "." + data.path, &data));
        } catch (const QuantraInvalidArgument& e) {
            // The library names the field relative to the swap; add which
            // swap.
            const std::string message = e.what();
            const std::string field = leadingField(message);
            const bool notFinite = message.find("is not finite") != std::string::npos;
            throw QuantraInvalidArgument(
                commodities::QualifySwapMessage(path, message),
                swapDetails(
                    notFinite ? "NotFinite" : "InvalidInput", i,
                    field.empty() ? path : path + "." + field));
        }
        if (reg.commodity.hasBuilds && !ctx.priceOnly) {
            result.quoteDeltas.push_back(quoteDeltas(result.swaps.back(), reg.commodity));
        }
    }
    result.hasBuilds = reg.commodity.hasBuilds;
    if (result.hasBuilds) {
        result.builds = reg.commodity.built;
    }
    return result;
}

} // namespace quantra
