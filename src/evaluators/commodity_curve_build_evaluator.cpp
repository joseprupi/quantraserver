#include "commodity_curve_build_evaluator.h"

#include "error.h"

#include <cstdio>
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
            } else {
                out += "{\"month\": " + jsonString(input.month.value_or("")) + "}";
            }
        }
        out += "]";
    }
    if (r.rule) {
        out += ", \"rule\": " + jsonString(commodities::CurveRuleName(*r.rule));
    }
    return out + "}";
}

CommodityCurveBuildResultSet CommodityCurveBuildEvaluator::evaluate(
    const CommodityCurveBuildInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    CommodityCurveBuildResultSet result;
    result.curves.reserve(inputs.builds.size());
    for (std::size_t i = 0; i < inputs.builds.size(); ++i) {
        ctx.budget.check();
        const auto& build = inputs.builds[i];
        const std::string path = "curves[" + std::to_string(i) + "]";
        const auto& data = build.contract;

        // Ids first: an unknown discounting curve is a 404.
        commodities::CurveBuildInputs core;
        core.interpolation = build.interpolation;
        core.extrapolateFlat = build.extrapolateFlat;
        core.rule = build.rule;
        core.includeDetails = inputs.includeDetails;
        core.averageQuotes = build.averageQuotes;
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

        BuiltCommodityCurveResult row;
        try {
            auto built = commodities::BuildCommodityCurve(*contract, core, inputs.asOf);
            row.months = std::move(built.months);
            row.report = std::move(built.report);
        } catch (const commodities::CurveBuildError& e) {
            // A refusal of the solve or the fill: the same message, plus its
            // data as the error's details.
            throw QuantraInvalidArgument(
                commodities::QualifyCurveBuildMessage(path, e.what()),
                CurveBuildRefusalJson(e.refusal(), i, path + "."));
        } catch (const QuantraInvalidArgument& e) {
            const std::string message = commodities::QualifyCurveBuildMessage(path, e.what());
            if (e.path().empty()) {
                QUANTRA_INVALID_ARGUMENT(message);
            }
            commodities::CurveBuildRefusal r;
            r.reason = commodities::CurveBuildRefusal::Reason::InvalidInput;
            r.path = e.path();
            throw QuantraInvalidArgument(message, CurveBuildRefusalJson(r, i, path + "."));
        }
        row.id = data.id;
        row.pricingCalendarWire = build.pricingCalendarWire;
        row.pricingHolidays = build.pricingHolidays;
        row.fixingsGiven = build.fixingsGiven;
        row.fixings = data.fixings;
        result.curves.push_back(std::move(row));
    }
    return result;
}

} // namespace quantra
