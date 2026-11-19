#include "commodity_curve_build_evaluator.h"

#include "error.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>

namespace quantra {

namespace {

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

std::string jsonNumber(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

} // namespace

std::string CurveBuildRefusalJson(
    const commodities::CurveBuildRefusal& r,
    std::optional<std::size_t> curve,
    const std::string& pathPrefix) {
    std::string out =
        "{\"reason\": " + jsonString(commodities::CurveBuildRefusalReasonName(r.reason));
    if (curve) {
        out += ", \"curve\": " + std::to_string(*curve);
    }
    if (!r.path.empty()) {
        out += ", \"path\": " + jsonString(pathPrefix + r.path);
    }
    if (r.averageQuote) {
        out += ", \"average_quote\": " + std::to_string(*r.averageQuote);
    }
    if (r.month) {
        out += ", \"month\": " + jsonString(*r.month);
    }
    if (!r.months.empty()) {
        out += ", \"months\": [";
        for (std::size_t k = 0; k < r.months.size(); ++k) {
            out += (k == 0 ? "" : ", ") + jsonString(r.months[k]);
        }
        out += "]";
    }
    if (r.impliedValue) {
        out += ", \"implied_value\": " + jsonNumber(*r.impliedValue);
    }
    if (r.quotedValue) {
        out += ", \"quoted_value\": " + jsonNumber(*r.quotedValue);
    }
    if (r.gap) {
        out += ", \"gap\": " + jsonNumber(*r.gap);
    }
    if (!r.dependsOn.empty()) {
        out += ", \"depends_on\": [";
        for (std::size_t k = 0; k < r.dependsOn.size(); ++k) {
            const auto& input = r.dependsOn[k];
            out += k == 0 ? "" : ", ";
            if (input.averageQuote) {
                out += "{\"average_quote\": " + std::to_string(*input.averageQuote) + "}";
            } else if (input.date) {
                out += "{\"date\": " + jsonString(*input.date) + "}";
            } else {
                out += "{\"month\": " + jsonString(input.month.value_or("")) + "}";
            }
        }
        out += "]";
    }
    if (r.rule) {
        out += ", \"rule\": " + jsonString(commodities::CurveRuleName(*r.rule));
    }
    if (r.date) {
        out += ", \"date\": " + jsonString(*r.date);
    }
    if (!r.dates.empty()) {
        out += ", \"dates\": [";
        for (std::size_t k = 0; k < r.dates.size(); ++k) {
            out += (k == 0 ? "" : ", ") + jsonString(r.dates[k]);
        }
        out += "]";
    }
    if (r.baseCurve) {
        out += ", \"base_curve\": " + std::to_string(*r.baseCurve);
    }
    if (r.baseMonth) {
        out += ", \"base_month\": " + jsonString(*r.baseMonth);
    }
    if (!r.cycle.empty()) {
        out += ", \"cycle\": [";
        for (std::size_t k = 0; k < r.cycle.size(); ++k) {
            out += (k == 0 ? "" : ", ") + std::to_string(r.cycle[k]);
        }
        out += "]";
    }
    return out + "}";
}

namespace {

[[noreturn]] void basisRefusal(
    commodities::CurveBuildRefusal::Reason reason,
    std::size_t curve,
    const std::string& message,
    std::optional<std::size_t> baseCurve,
    const std::vector<std::size_t>& cycle) {
    commodities::CurveBuildRefusal r;
    r.reason = reason;
    r.path = "basis.base";
    r.baseCurve = baseCurve;
    r.cycle = cycle;
    const std::string path = "curves[" + std::to_string(curve) + "]";
    const std::string text = path + ".basis.base: " + message;
    if (reason == commodities::CurveBuildRefusal::Reason::UnknownBase) {
        throw QuantraNotFound(text, CurveBuildRefusalJson(r, curve, path + "."));
    }
    throw QuantraInvalidArgument(text, CurveBuildRefusalJson(r, curve, path + "."));
}

/// The order the entries are built in: every base before the entries that
/// read it, otherwise request order. Resolves each basis's base index.
std::vector<std::size_t> buildOrder(
    const CommodityCurveBuildInputs& inputs, std::vector<std::optional<std::size_t>>& baseOf) {
    using Reason = commodities::CurveBuildRefusal::Reason;
    const std::size_t n = inputs.builds.size();
    baseOf.assign(n, std::nullopt);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& build = inputs.builds[i];
        if (!build.basis) {
            continue;
        }
        std::vector<std::size_t> named;
        bool spotNamed = false;
        for (std::size_t j = 0; j < n; ++j) {
            const auto& other = inputs.builds[j];
            if (other.contract && other.contract->id == build.baseId) {
                named.push_back(j);
            } else if (other.spot && other.spot->id == build.baseId) {
                spotNamed = true;
            }
        }
        if (named.empty() && spotNamed) {
            basisRefusal(
                Reason::BaseNotContract, i,
                "'" + build.baseId + "' is a spot price build; a base must be a contract",
                std::nullopt, {});
        }
        if (named.empty()) {
            basisRefusal(
                Reason::UnknownBase, i, "no contract build in curves has id '" + build.baseId + "'",
                std::nullopt, {});
        }
        if (named.size() > 1) {
            basisRefusal(
                Reason::BaseNotContract, i,
                "'" + build.baseId + "' names " + std::to_string(named.size()) +
                    " contract builds; ids of bases must be unique",
                std::nullopt, {});
        }
        if (named[0] == i) {
            const std::vector<std::size_t> self{i};
            basisRefusal(Reason::BasisCycle, i, "a contract cannot be its own base", i, self);
        }
        baseOf[i] = named[0];
    }
    std::vector<int> state(n, 0); // 0 new, 1 on the stack, 2 done
    std::vector<std::size_t> order;
    std::vector<std::size_t> stack;
    std::function<void(std::size_t)> visit = [&](std::size_t i) {
        if (state[i] == 2) {
            return;
        }
        if (state[i] == 1) {
            auto it = std::find(stack.begin(), stack.end(), i);
            std::vector<std::size_t> cycle(it, stack.end());
            std::string names;
            for (const std::size_t c : cycle) {
                names += "curves[" + std::to_string(c) + "] -> ";
            }
            names += "curves[" + std::to_string(i) + "]";
            basisRefusal(
                Reason::BasisCycle, cycle.front(), "the bases form a cycle: " + names,
                baseOf[cycle.front()], cycle);
        }
        state[i] = 1;
        stack.push_back(i);
        if (baseOf[i]) {
            visit(*baseOf[i]);
        }
        stack.pop_back();
        state[i] = 2;
        order.push_back(i);
    };
    for (std::size_t i = 0; i < n; ++i) {
        visit(i);
    }
    return order;
}

} // namespace

CommodityCurveBuildResultSet CommodityCurveBuildEvaluator::evaluate(
    const CommodityCurveBuildInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    CommodityCurveBuildResultSet result;
    std::vector<std::optional<std::size_t>> baseIndex;
    const auto order = buildOrder(inputs, baseIndex);
    std::vector<std::optional<BuiltCommodityCurveResult>> rows(inputs.builds.size());
    std::vector<std::shared_ptr<const commodities::FuturesContract>> builtContracts(
        inputs.builds.size());
    for (const std::size_t i : order) {
        ctx.budget.check();
        const auto& build = inputs.builds[i];
        const std::string path = "curves[" + std::to_string(i) + "]";

        // Ids first: an unknown discounting curve is a 404.
        commodities::CurveBuildInputs core;
        core.interpolation = build.interpolation;
        core.extrapolateFlat = build.extrapolateFlat;
        core.rule = build.rule;
        core.includeDetails = inputs.includeDetails;
        core.averageQuotes = build.averageQuotes;
        if (build.basis) {
            core.basis = build.basis;
            core.basis->baseCurve = *baseIndex[i];
            core.basis->base = builtContracts[*baseIndex[i]];
        }
        for (std::size_t k = 0; k < core.averageQuotes.size(); ++k) {
            const auto& id = build.discountingCurveIds[k];
            if (!id) {
                continue;
            }
            auto curve = reg.rates.curves.find(*id);
            if (curve == reg.rates.curves.end() || !curve->second) {
                QUANTRA_NOT_FOUND(
                    path + ".average_quotes[" + std::to_string(k) +
                    "].discounting_curve: discounting curve not found: " + *id);
            }
            core.averageQuotes[k].discountCurve =
                QuantLib::Handle<QuantLib::YieldTermStructure>(*curve->second);
        }

        BuiltCommodityCurveResult row;
        auto rethrow = [&](const QuantraInvalidArgument& e) {
            const std::string message = commodities::QualifyCurveBuildMessage(path, e.what());
            if (const auto* refused = dynamic_cast<const commodities::CurveBuildError*>(&e)) {
                // A refusal of the solve or the fill: the same message, plus
                // its data as the error's details.
                throw QuantraInvalidArgument(
                    message, CurveBuildRefusalJson(refused->refusal(), i, path + "."));
            }
            if (e.path().empty()) {
                QUANTRA_INVALID_ARGUMENT(message);
            }
            commodities::CurveBuildRefusal r;
            r.reason = commodities::CurveBuildRefusal::Reason::InvalidInput;
            r.path = e.path();
            throw QuantraInvalidArgument(message, CurveBuildRefusalJson(r, i, path + "."));
        };
        if (build.spot) {
            const auto& sdata = *build.spot;
            commodities::SpotCurveSpec spec{
                sdata.id,     sdata.calendar, sdata.interpolation, sdata.extrapolateFlat,
                sdata.points, sdata.fixings};
            try {
                auto built = commodities::BuildSpotCurve(spec, core, inputs.asOf);
                row.points = std::move(built.points);
                row.report = std::move(built.report);
            } catch (const QuantraInvalidArgument& e) {
                rethrow(e);
            }
            row.isSpot = true;
            row.id = sdata.id;
            row.pricingCalendarWire = build.pricingCalendarWire;
            row.pricingHolidays = build.pricingHolidays;
            row.fixingsGiven = build.fixingsGiven;
            row.spotFixings = sdata.fixings;
            row.spotInterpolationWire = build.spotInterpolationWire;
            row.extrapolateFlat = sdata.extrapolateFlat;
            rows[i] = std::move(row);
            continue;
        }
        const auto& data = *build.contract;

        // The contract's value rules (months, last trading dates, prices,
        // fixings) are its constructor's; its messages name the contract
        // and the month or fixing index.
        std::optional<commodities::FuturesContract> contract;
        try {
            contract.emplace(data.id, data.calendar, data.months, data.fixings);
        } catch (const QuantraInvalidArgument& e) {
            if (e.path().empty()) {
                QUANTRA_INVALID_ARGUMENT(path + ".contract: " + e.what());
            }
            // The constructor names the field relative to the contract.
            commodities::CurveBuildRefusal r;
            r.reason = commodities::CurveBuildRefusal::Reason::InvalidInput;
            r.path = e.path();
            throw QuantraInvalidArgument(
                path + ".contract: " + e.what(), CurveBuildRefusalJson(r, i, path + ".contract."));
        }

        try {
            auto built = commodities::BuildCommodityCurve(*contract, core, inputs.asOf);
            row.months = std::move(built.months);
            row.report = std::move(built.report);
        } catch (const QuantraInvalidArgument& e) {
            rethrow(e);
        }
        row.id = data.id;
        row.pricingCalendarWire = build.pricingCalendarWire;
        row.pricingHolidays = build.pricingHolidays;
        row.fixingsGiven = build.fixingsGiven;
        row.fixings = data.fixings;
        // The built contract, for the entries that use it as their base.
        builtContracts[i] = std::make_shared<const commodities::FuturesContract>(
            data.id, data.calendar, row.months, data.fixings);
        rows[i] = std::move(row);
    }
    result.curves.reserve(rows.size());
    for (auto& row : rows) {
        result.curves.push_back(std::move(*row));
    }
    return result;
}

} // namespace quantra
