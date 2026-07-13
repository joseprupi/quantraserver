#include "callable_fixed_rate_bond_mapper.h"

#include "callable_fixed_rate_bond_parser.h"
#include "error.h"

namespace quantra {

CallableFixedRateBondInputs CallableFixedRateBondMapper::toInputs(
    const quantra::PriceCallableFixedRateBondRequest* req) const {

    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceCallableFixedRateBondRequest is null");
    }
    const auto* bondPricings = req->bonds();
    if (bondPricings == nullptr || bondPricings->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceCallableFixedRateBondRequest.bonds is required and must be non-empty");
    }

    CallableFixedRateBondParser bondParser;
    CallableFixedRateBondInputs inputs;
    inputs.trades.reserve(bondPricings->size());

    for (auto it = bondPricings->begin(); it != bondPricings->end(); ++it) {
        if (it->callable_fixed_rate_bond() == nullptr) {
            QUANTRA_INVALID_ARGUMENT(
                "PriceCallableFixedRateBond.callable_fixed_rate_bond is required");
        }
        if (it->discounting_curve() == nullptr || it->discounting_curve()->str().empty()) {
            QUANTRA_INVALID_ARGUMENT("PriceCallableFixedRateBond.discounting_curve is required");
        }
        if (it->model() == nullptr || it->model()->str().empty()) {
            QUANTRA_INVALID_ARGUMENT("PriceCallableFixedRateBond.model is required");
        }

        CallableFixedRateBondTrade trade;
        trade.bond = bondParser.parse(it->callable_fixed_rate_bond());
        trade.discountingCurveId = it->discounting_curve()->str();
        trade.modelId = it->model()->str();

        // Optional tree_steps: absent => server default; present => clamped
        // into [kMinTreeSteps, kMaxTreeSteps]. A clamp is not an error — a
        // caller can only ever ask for LESS work than the server ceiling.
        if (it->tree_steps().has_value()) {
            int steps = it->tree_steps().value();
            if (steps < kMinTreeSteps) steps = kMinTreeSteps;
            if (steps > kMaxTreeSteps) steps = kMaxTreeSteps;
            trade.treeSteps = steps;
        } else {
            trade.treeSteps = kDefaultTreeSteps;
        }

        inputs.trades.push_back(std::move(trade));
    }

    return inputs;
}

flatbuffers::Offset<quantra::PriceCallableFixedRateBondResponse>
CallableFixedRateBondMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const CallableFixedRateBondResult& result) const {

    std::vector<flatbuffers::Offset<quantra::CallableFixedRateBondResponse>> bondsVector;
    bondsVector.reserve(result.bonds.size());

    for (const auto& bond : result.bonds) {
        auto settlement = builder.CreateString(bond.settlementDate);
        quantra::CallableFixedRateBondResponseBuilder rb(builder);
        rb.add_npv(bond.npv);
        rb.add_clean_price(bond.cleanPrice);
        rb.add_dirty_price(bond.dirtyPrice);
        rb.add_settlement_date(settlement);
        bondsVector.push_back(rb.Finish());
    }

    auto bonds = builder.CreateVector(bondsVector);
    quantra::PriceCallableFixedRateBondResponseBuilder rb(builder);
    rb.add_bonds(bonds);
    return rb.Finish();
}

} // namespace quantra
