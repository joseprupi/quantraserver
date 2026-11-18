#include "commodity_curve_build.h"

#include "commodity_linear_system.h"
#include "date_convert.h"
#include "error.h"

#include <ql/math/interpolation.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace quantra { namespace commodities {

namespace {

[[noreturn]] void refuse(const std::string& message, CurveBuildRefusal refusal) {
    throw CurveBuildError(message, std::move(refusal));
}

CurveBuildRefusal refusalOf(CurveBuildRefusal::Reason reason) {
    CurveBuildRefusal r;
    r.reason = reason;
    return r;
}

std::string monthField(const std::vector<ContractMonth>& months, std::size_t i) {
    return "contract.months[" + std::to_string(i) + "] (" + months[i].month + ")";
}

std::string formatPrice(double value) {
    std::ostringstream os;
    os.precision(17);
    os << value;
    return os.str();
}

/// Reads the quoted live months between the first and the last quote with one
/// of the six methods. Linear, BackwardFlat and ForwardFlat are the spot
/// price's readings (SpotPrice, calendar days); LogLinear and the two cubics
/// are the library's interpolations on x = calendar days from the valuation
/// date / 365 (an affine change of the axis leaves every method unchanged).
class QuoteLine {
public:
    QuoteLine(
        const FuturesContract& contract,
        const std::vector<SpotPoint>& quoted,
        CurveInterpolation method,
        const QuantLib::Date& asOf)
        : asOf_(asOf) {
        switch (method) {
            case CurveInterpolation::Linear:
            case CurveInterpolation::BackwardFlat:
            case CurveInterpolation::ForwardFlat: {
                const SpotInterpolation spot =
                    method == CurveInterpolation::Linear         ? SpotInterpolation::Linear
                    : method == CurveInterpolation::BackwardFlat ? SpotInterpolation::BackwardFlat
                                                                 : SpotInterpolation::ForwardFlat;
                spot_.emplace(
                    contract.id(), contract.pricingCalendar(), spot, false, quoted,
                    std::vector<SpotFixing>{});
                return;
            }
            default: break;
        }
        for (const auto& q : quoted) {
            x_.push_back(time(q.date));
            y_.push_back(q.price);
        }
        try {
            if (method == CurveInterpolation::LogLinear) {
                curve_ = QuantLib::LogLinearInterpolation(x_.begin(), x_.end(), y_.begin());
            } else {
                const auto approx = method == CurveInterpolation::KrugerCubic
                                        ? QuantLib::CubicInterpolation::Kruger
                                        : QuantLib::CubicInterpolation::Spline;
                curve_ = QuantLib::CubicInterpolation(
                    x_.begin(), x_.end(), y_.begin(), approx, false,
                    QuantLib::CubicInterpolation::SecondDerivative, 0.0,
                    QuantLib::CubicInterpolation::SecondDerivative, 0.0);
            }
            curve_.update();
        } catch (const std::exception&) {
            // Callers build a line only with two quotes or more, which every
            // method accepts; anything else is reported without library text.
            QUANTRA_INVALID_ARGUMENT(
                "interpolation " + CurveInterpolationName(method) + " cannot be built on " +
                std::to_string(quoted.size()) + " quoted live months");
        }
    }

    /// The line on `date`, between the first and the last quote (both
    /// included).
    double read(const QuantLib::Date& date) const {
        if (spot_) {
            return spot_->forward(date);
        }
        return curve_(time(date));
    }

private:
    double time(const QuantLib::Date& date) const {
        return static_cast<double>(date - asOf_) / 365.0;
    }

    QuantLib::Date asOf_;
    std::optional<SpotPrice> spot_;
    std::vector<double> x_;
    std::vector<double> y_;
    QuantLib::Interpolation curve_;
};

} // namespace

std::string CurveInterpolationName(CurveInterpolation method) {
    switch (method) {
        case CurveInterpolation::Linear: return "Linear";
        case CurveInterpolation::LogLinear: return "LogLinear";
        case CurveInterpolation::BackwardFlat: return "BackwardFlat";
        case CurveInterpolation::ForwardFlat: return "ForwardFlat";
        case CurveInterpolation::KrugerCubic: return "KrugerCubic";
        case CurveInterpolation::NaturalCubic: return "NaturalCubic";
    }
    return "unknown";
}

FuturesCurveBuildResult BuildFuturesCurve(
    const FuturesContract& contract,
    std::optional<CurveInterpolation> interpolation,
    bool extrapolateFlat,
    const QuantLib::Date& asOf) {
    FuturesCurveBuildResult out;
    out.months = contract.months();
    const auto& months = out.months;

    // Live months: read by a swap from today's prices. Split into quoted
    // (the points) and missing (to fill).
    std::vector<SpotPoint> quoted;
    std::vector<std::size_t> quotedIndex;
    std::vector<std::size_t> missing;
    for (std::size_t i = 0; i < months.size(); ++i) {
        if (months[i].lastTradeDate < asOf) {
            continue;
        }
        if (months[i].price) {
            quoted.push_back({months[i].lastTradeDate, *months[i].price});
            quotedIndex.push_back(i);
        } else {
            missing.push_back(i);
        }
    }
    if (missing.empty()) {
        return out;
    }

    std::string missingList;
    for (std::size_t k = 0; k < missing.size(); ++k) {
        missingList += (k == 0 ? "" : ", ") + months[missing[k]].month;
    }
    std::vector<std::string> missingMonths;
    for (const std::size_t i : missing) {
        missingMonths.push_back(months[i].month);
    }
    if (!interpolation) {
        auto r = refusalOf(CurveBuildRefusal::Reason::InterpolationRequired);
        r.path = "interpolation";
        r.months = missingMonths;
        refuse(
            "interpolation is required: live months without a price (" + missingList +
                ") are filled from the quoted live months",
            r);
    }
    if (quoted.empty()) {
        auto r = refusalOf(CurveBuildRefusal::Reason::NoPricedMonth);
        r.path = "contract.months";
        r.months = missingMonths;
        refuse(
            "contract.months: no live month has a price (last_trade_date on or after " +
                DateToIso(asOf) + "), so " + missingList + " cannot be filled",
            r);
    }
    if (*interpolation == CurveInterpolation::LogLinear) {
        for (std::size_t k = 0; k < quoted.size(); ++k) {
            if (!(quoted[k].price > 0.0)) {
                auto r = refusalOf(CurveBuildRefusal::Reason::NonPositivePrice);
                r.path = "contract.months[" + std::to_string(quotedIndex[k]) + "].price";
                r.month = months[quotedIndex[k]].month;
                refuse(
                    monthField(months, quotedIndex[k]) + ": price " + formatPrice(quoted[k].price) +
                        " must be > 0 for interpolation LogLinear (it reads log prices)",
                    r);
            }
        }
    }

    const QuantLib::Date first = quoted.front().date;
    const QuantLib::Date last = quoted.back().date;

    // A missing month strictly between two quotes needs the line; the last
    // trading dates are strictly ascending (contract rule), so a missing
    // month never sits on a quote and two quotes or more exist.
    std::optional<QuoteLine> line;
    for (const std::size_t i : missing) {
        const QuantLib::Date date = months[i].lastTradeDate;
        double price = 0.0;
        if (date < first || date > last) {
            if (!extrapolateFlat) {
                auto r = refusalOf(CurveBuildRefusal::Reason::MonthOutsideRange);
                r.path = "contract.months[" + std::to_string(i) + "]";
                r.month = months[i].month;
                for (const std::size_t j : missing) {
                    const QuantLib::Date d = months[j].lastTradeDate;
                    if (d < first || d > last) {
                        r.months.push_back(months[j].month);
                    }
                }
                refuse(
                    monthField(months, i) + ": last_trade_date " + DateToIso(date) + " is " +
                        (date < first ? "before the first" : "after the last") +
                        " quoted live month (" + DateToIso(first) + " to " + DateToIso(last) +
                        ") and extrapolate_flat is false",
                    r);
            }
            // Flat: the nearest quote, never a method continued.
            price = date < first ? quoted.front().price : quoted.back().price;
            out.extrapolatedMonths.push_back(months[i].month);
        } else {
            if (!line) {
                line.emplace(contract, quoted, *interpolation, asOf);
            }
            price = line->read(date);
        }
        if (!std::isfinite(price)) {
            auto r = refusalOf(CurveBuildRefusal::Reason::NotFinite);
            r.path = "contract.months[" + std::to_string(i) + "]";
            r.month = months[i].month;
            refuse(monthField(months, i) + ": the filled price is not finite", r);
        }
        out.months[i].price = price;
        out.interpolatedMonths.push_back(months[i].month);
    }
    return out;
}

std::string CurveBuildRefusalReasonName(CurveBuildRefusal::Reason reason) {
    switch (reason) {
        case CurveBuildRefusal::Reason::QuoteConflict: return "QuoteConflict";
        case CurveBuildRefusal::Reason::RuleRequired: return "RuleRequired";
        case CurveBuildRefusal::Reason::FlatCannotResolve: return "FlatCannotResolve";
        case CurveBuildRefusal::Reason::SmoothNotUnique: return "SmoothNotUnique";
        case CurveBuildRefusal::Reason::MonthOutsideRange: return "MonthOutsideRange";
        case CurveBuildRefusal::Reason::QuoteHasNoLiveMonth: return "QuoteHasNoLiveMonth";
        case CurveBuildRefusal::Reason::NonPositivePrice: return "NonPositivePrice";
        case CurveBuildRefusal::Reason::InterpolationRequired: return "InterpolationRequired";
        case CurveBuildRefusal::Reason::NoPricedMonth: return "NoPricedMonth";
        case CurveBuildRefusal::Reason::NotFinite: return "NotFinite";
        case CurveBuildRefusal::Reason::InvalidInput: return "InvalidInput";
    }
    return "Unknown";
}

std::string CurveRuleName(CurveRule rule) {
    return rule == CurveRule::Flat ? "Flat" : "Smooth";
}

namespace {

std::string quotePath(std::size_t k) {
    return "average_quotes[" + std::to_string(k) + "]";
}

/// A swap pricer message about the quote's swap, renamed to the quote: the
/// legs' fields are the quote's own (`leg2.periods[3] ...` becomes
/// `average_quotes[1].periods[3] ...`).
std::string quoteMessage(std::size_t k, const std::string& message) {
    for (const char* leg : {"leg1.", "leg2."}) {
        if (message.rfind(leg, 0) == 0) {
            return quotePath(k) + "." + message.substr(5);
        }
    }
    return quotePath(k) + ": " + message;
}

/// The two legs of a quote's swap: leg 1 pays the fixed `price`, leg 2
/// receives the contract's average. Value rules on the quote are checked
/// here.
std::pair<CommoditySwapLeg, CommoditySwapLeg> quoteLegs(
    const AverageQuote& q,
    std::size_t k,
    std::shared_ptr<const FuturesContract> contract,
    double price,
    const QuantLib::Date& asOf) {
    const std::string path = quotePath(k);
    ValidatePeriods(q.periods, path + ".periods");
    ValidateNearbyAndRoll(q.nearby, q.rollDaysBeforeExpiry, path);
    const bool several = q.periods.size() > 1;
    auto require = [&](bool present, const std::string& field) {
        if (several && !present) {
            QuantraInvalidArgumentAt(
                path + "." + field, path + "." + field + " is required for a quote of " +
                                        std::to_string(q.periods.size()) +
                                        " periods (each period is paid and " +
                                        "discounted on its own)");
        }
    };
    require(q.quantityBasis.has_value(), "quantity_basis");
    require(q.paymentCalendar.has_value(), "payment_calendar");
    require(q.paymentLag.has_value(), "payment_lag");
    require(q.paymentConvention.has_value(), "payment_convention");
    require(!q.discountCurve.empty(), "discounting_curve");

    CommoditySwapLeg fixed;
    fixed.payReceive = PayReceive::Pay;
    fixed.periods = q.periods;
    fixed.quantity.value = 1.0;
    fixed.quantity.basis = q.quantityBasis.value_or(QuantityBasis::PerPeriod);
    FixedPrice fp;
    fp.price = price;
    fixed.price = fp;
    // One period without payment terms: paid the day after its last day and
    // not discounted; both cancel in the fair price.
    fixed.paymentCalendar = q.paymentCalendar.value_or(QuantLib::NullCalendar());
    fixed.paymentLag = q.paymentLag.value_or(1);
    fixed.paymentConvention = q.paymentConvention.value_or(QuantLib::Following);
    fixed.discountCurve =
        q.discountCurve.empty()
            ? QuantLib::Handle<QuantLib::YieldTermStructure>(
                  std::make_shared<QuantLib::FlatForward>(asOf, 0.0, QuantLib::Actual365Fixed()))
            : q.discountCurve;
    CommoditySwapLeg floating = fixed;
    floating.payReceive = PayReceive::Receive;
    FuturesFloatingPrice ff;
    ff.contract = std::move(contract);
    ff.nearby = q.nearby;
    ff.rollDaysBeforeExpiry = q.rollDaysBeforeExpiry;
    floating.price = ff;
    return {fixed, floating};
}

CommoditySwapResult priceQuote(
    const AverageQuote& q,
    std::size_t k,
    const std::shared_ptr<const FuturesContract>& contract,
    double price,
    const QuantLib::Date& asOf) {
    const auto legs = quoteLegs(q, k, contract, price, asOf);
    try {
        return PriceCommoditySwap(legs.first, legs.second, asOf);
    } catch (const QuantraInvalidArgument& e) {
        QUANTRA_INVALID_ARGUMENT(quoteMessage(k, e.what()));
    }
    return {}; // unreachable
}

/// One average quote as an equation: quote = constant + sum weight x price.
struct QuoteEquation {
    std::map<std::string, double> weights; // by month
    double constant = 0.0;
};

QuoteEquation quoteEquation(
    const AverageQuote& q,
    std::size_t k,
    const std::shared_ptr<const FuturesContract>& placeholder,
    const QuantLib::Date& asOf) {
    // Fixed price 0 and every live month at 0: the NPV is the realised part.
    const auto r = priceQuote(q, k, placeholder, 0.0, asOf);
    double annuity = 0.0;
    for (const auto& flow : r.legs[0].flows) {
        annuity += flow.quantity * flow.discount;
    }
    QuoteEquation eq;
    if (annuity > 0.0) {
        for (const auto& d : r.deltas) {
            if (d.delta != 0.0) {
                eq.weights[d.month] = d.delta / annuity;
            }
        }
    }
    if (eq.weights.empty()) {
        auto r = refusalOf(CurveBuildRefusal::Reason::QuoteHasNoLiveMonth);
        r.path = quotePath(k);
        r.averageQuote = k;
        refuse(
            quotePath(k) +
                ": no live month: every pricing day it averages is a fixing or every period is "
                "already paid, so it constrains no price",
            r);
    }
    eq.constant = r.legs[1].npv / annuity;
    return eq;
}

std::string formatNumber(double value) {
    std::ostringstream os;
    os.precision(12);
    os << value;
    return os.str();
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        out += (i == 0 ? "" : ", ") + names[i];
    }
    return out;
}

/// Second differences over n unknowns in index order ((n-2) x n).
std::vector<Vector> secondDifferences(std::size_t n) {
    std::vector<Vector> d;
    for (std::size_t i = 1; i + 1 < n; ++i) {
        Vector row(n, 0.0);
        row[i - 1] = 1.0;
        row[i] = -2.0;
        row[i + 1] = 1.0;
        d.push_back(std::move(row));
    }
    return d;
}

Vector multiply(const std::vector<Vector>& m, const Vector& v) {
    Vector out;
    out.reserve(m.size());
    for (const auto& row : m) {
        out.push_back(Dot(row, v));
    }
    return out;
}

} // namespace

CommodityCurveBuildResult BuildCommodityCurve(
    const FuturesContract& contract, const CurveBuildInputs& inputs, const QuantLib::Date& asOf) {
    const auto& months = contract.months();
    const std::size_t nm = months.size();
    std::vector<std::size_t> live;
    std::map<std::string, std::size_t> monthIndex;
    for (std::size_t i = 0; i < nm; ++i) {
        monthIndex[months[i].month] = i;
        if (months[i].lastTradeDate >= asOf) {
            live.push_back(i);
        }
    }
    const auto& quotes = inputs.averageQuotes;
    for (std::size_t k = 0; k < quotes.size(); ++k) {
        if (!std::isfinite(quotes[k].price)) {
            auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
            r.path = quotePath(k) + ".price";
            r.averageQuote = k;
            refuse(quotePath(k) + ".price must be finite", r);
        }
    }

    // Every live month at 0: the quote equations from one swap evaluation each.
    std::vector<std::optional<double>> zero(nm);
    for (std::size_t i = 0; i < nm; ++i) {
        zero[i] = months[i].price;
    }
    for (const std::size_t i : live) {
        zero[i] = 0.0;
    }
    const auto placeholder = std::make_shared<const FuturesContract>(contract.withPrices(zero));
    std::vector<QuoteEquation> equations;
    equations.reserve(quotes.size());
    for (std::size_t k = 0; k < quotes.size(); ++k) {
        try {
            equations.push_back(quoteEquation(quotes[k], k, placeholder, asOf));
        } catch (const CurveBuildError&) {
            throw;
        } catch (const QuantraInvalidArgument& e) {
            if (e.path().empty()) {
                throw;
            }
            // A quote value a rule refuses, with the field the rule named.
            auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
            r.path = e.path();
            r.averageQuote = k;
            refuse(e.what(), r);
        }
    }

    // Unknowns: live months with a price or a weight, in month order.
    std::vector<std::size_t> unknowns; // month indexes
    std::map<std::size_t, std::size_t> column;
    for (const std::size_t i : live) {
        bool touched = months[i].price.has_value();
        for (const auto& eq : equations) {
            touched = touched || eq.weights.count(months[i].month) > 0;
        }
        if (touched) {
            column[i] = unknowns.size();
            unknowns.push_back(i);
        }
    }
    const std::size_t n = unknowns.size();

    // Equations: direct prices (month order), then average quotes.
    std::vector<Vector> rows;
    Vector rhs;
    Vector quoted;                     // the quoted value, for the tolerance
    std::vector<std::string> rowNames; // for conflict messages
    std::vector<std::optional<std::size_t>> rowQuote;
    for (const std::size_t i : unknowns) {
        if (months[i].price) {
            Vector row(n, 0.0);
            row[column[i]] = 1.0;
            rows.push_back(row);
            rhs.push_back(*months[i].price);
            quoted.push_back(*months[i].price);
            rowNames.push_back(months[i].month);
            rowQuote.push_back(std::nullopt);
        }
    }
    for (std::size_t k = 0; k < quotes.size(); ++k) {
        Vector row(n, 0.0);
        for (const auto& w : equations[k].weights) {
            row[column[monthIndex[w.first]]] = w.second;
        }
        rows.push_back(row);
        rhs.push_back(quotes[k].price - equations[k].constant);
        quoted.push_back(quotes[k].price);
        rowNames.push_back(quotePath(k));
        rowQuote.push_back(k);
    }

    CommodityCurveBuildResult result;
    auto& report = result.report;
    report.unknowns = static_cast<int>(n);
    report.equations = static_cast<int>(rows.size());
    report.rule = inputs.rule;
    report.averageQuotes.resize(quotes.size());

    EquationSpace space(n);
    for (std::size_t e = 0; e < rows.size(); ++e) {
        const auto added = space.add(rows[e], rhs[e]);
        if (added.independent) {
            continue;
        }
        const std::size_t k = *rowQuote[e]; // direct prices are never dependent
        const double implied = added.implied + equations[k].constant;
        const double gap = quotes[k].price - implied;
        if (std::fabs(gap) <= 1e-8 * std::max(1.0, std::fabs(quotes[k].price))) {
            report.averageQuotes[k].status = QuoteStatus::Redundant;
            continue;
        }
        std::vector<std::string> involved;
        auto r = refusalOf(CurveBuildRefusal::Reason::QuoteConflict);
        r.path = quotePath(k);
        r.averageQuote = k;
        r.impliedValue = implied;
        r.quotedValue = quotes[k].price;
        r.gap = gap;
        for (std::size_t j = 0; j < added.combination.size(); ++j) {
            if (std::fabs(added.combination[j]) > 1e-12) {
                involved.push_back(rowNames[j]);
                CurveBuildRefusal::Input input;
                if (rowQuote[j]) {
                    input.averageQuote = *rowQuote[j];
                } else {
                    input.month = rowNames[j];
                }
                r.dependsOn.push_back(input);
            }
        }
        refuse(
            quotePath(k) + ": conflicts with the other quotes: they imply " +
                formatNumber(implied) + ", it quotes " + formatNumber(quotes[k].price) +
                ", a gap of " + formatNumber(gap) + " (it is determined by " + joinNames(involved) +
                ")",
            r);
    }
    report.rank = static_cast<int>(space.rank());

    std::vector<bool> determined(n);
    std::vector<std::string> undetermined;
    for (std::size_t c = 0; c < n; ++c) {
        Vector e(n, 0.0);
        e[c] = 1.0;
        determined[c] = space.spans(e);
        if (!determined[c]) {
            undetermined.push_back(months[unknowns[c]].month);
        }
    }

    // The solve as a linear function of the right-hand sides.
    std::function<Vector(const Vector&)> solve;
    if (space.rank() == n) {
        report.status = CurveStatus::Unique;
        solve = [&space](const Vector& b) { return space.solve(b); };
    } else {
        report.status = CurveStatus::Underdetermined;
        if (!inputs.rule) {
            auto r = refusalOf(CurveBuildRefusal::Reason::RuleRequired);
            r.path = "rule";
            r.months = undetermined;
            refuse(
                "rule is required: the quotes do not determine " + joinNames(undetermined) + " (" +
                    std::to_string(n) + " months, " + std::to_string(space.rank()) +
                    " independent quotes); give Flat or Smooth",
                r);
        }
        if (*inputs.rule == CurveRule::Flat) {
            // Months touched by the same equations share one price.
            std::map<std::vector<std::size_t>, std::size_t> groupOf;
            std::vector<std::size_t> group(n);
            for (std::size_t c = 0; c < n; ++c) {
                std::vector<std::size_t> signature;
                for (std::size_t e = 0; e < rows.size(); ++e) {
                    if (rows[e][c] != 0.0) {
                        signature.push_back(e);
                    }
                }
                auto it = groupOf.emplace(signature, groupOf.size()).first;
                group[c] = it->second;
            }
            const std::size_t g = groupOf.size();
            auto reduced = std::make_shared<EquationSpace>(g);
            for (std::size_t e = 0; e < rows.size(); ++e) {
                Vector row(g, 0.0);
                for (std::size_t c = 0; c < n; ++c) {
                    row[group[c]] += rows[e][c];
                }
                const auto added = reduced->add(row, rhs[e]);
                if (!added.independent && std::fabs(rhs[e] - added.implied) >
                                              1e-8 * std::max(1.0, std::fabs(quoted[e]))) {
                    auto r = refusalOf(CurveBuildRefusal::Reason::FlatCannotResolve);
                    r.path = "rule";
                    r.rule = CurveRule::Flat;
                    r.months = undetermined;
                    if (rowQuote[e]) {
                        r.averageQuote = *rowQuote[e];
                    } else {
                        r.month = rowNames[e];
                    }
                    r.gap = rhs[e] - added.implied;
                    refuse(
                        "rule Flat cannot resolve these quotes: with one price per group of "
                        "months touched by the same quotes, " +
                            rowNames[e] + " is no longer repriced (gap " +
                            formatNumber(rhs[e] - added.implied) + ")",
                        r);
                }
            }
            if (reduced->rank() < g) {
                auto r = refusalOf(CurveBuildRefusal::Reason::FlatCannotResolve);
                r.path = "rule";
                r.rule = CurveRule::Flat;
                r.months = undetermined;
                refuse(
                    "rule Flat cannot resolve these quotes: one price per group of months "
                    "touched by the same quotes still leaves " +
                        joinNames(undetermined) + " undetermined",
                    r);
            }
            solve = [reduced, group, n](const Vector& b) {
                const Vector y = reduced->solve(b);
                Vector x(n);
                for (std::size_t c = 0; c < n; ++c) {
                    x[c] = y[group[c]];
                }
                return x;
            };
        } else {
            const auto basis = space.nullSpace();
            const auto d = secondDifferences(n);
            // M = D N, one column per null-space vector.
            std::vector<Vector> m(d.size(), Vector(basis.size(), 0.0));
            for (std::size_t r = 0; r < d.size(); ++r) {
                for (std::size_t j = 0; j < basis.size(); ++j) {
                    m[r][j] = Dot(d[r], basis[j]);
                }
            }
            std::vector<Vector> h(basis.size(), Vector(basis.size(), 0.0));
            for (std::size_t a = 0; a < basis.size(); ++a) {
                for (std::size_t b = 0; b < basis.size(); ++b) {
                    for (std::size_t r = 0; r < d.size(); ++r) {
                        h[a][b] += m[r][a] * m[r][b];
                    }
                }
            }
            EquationSpace columns(d.size());
            for (std::size_t j = 0; j < basis.size(); ++j) {
                Vector col(d.size());
                for (std::size_t r = 0; r < d.size(); ++r) {
                    col[r] = m[r][j];
                }
                columns.add(col, 0.0);
            }
            Vector probe;
            if (columns.rank() < basis.size() ||
                !SolveSquare(h, Vector(basis.size(), 0.0), probe)) {
                auto r = refusalOf(CurveBuildRefusal::Reason::SmoothNotUnique);
                r.path = "rule";
                r.rule = CurveRule::Smooth;
                r.months = undetermined;
                refuse(
                    "rule Smooth has no unique solution: the quotes leave " +
                        joinNames(undetermined) +
                        " free in a way the second differences do not fix (they need a level "
                        "and a slope); use Flat",
                    r);
            }
            solve = [&space, basis, d, m, h, n](const Vector& b) {
                Vector x = space.solve(b);
                const Vector dx = multiply(d, x);
                Vector g(basis.size(), 0.0);
                for (std::size_t j = 0; j < basis.size(); ++j) {
                    for (std::size_t r = 0; r < d.size(); ++r) {
                        g[j] -= m[r][j] * dx[r];
                    }
                }
                Vector z;
                SolveSquare(h, g, z);
                for (std::size_t j = 0; j < basis.size(); ++j) {
                    for (std::size_t c = 0; c < n; ++c) {
                        x[c] += z[j] * basis[j][c];
                    }
                }
                return x;
            };
        }
    }
    const Vector x = solve(rhs);

    // The determined months are the nodes of the fill.
    std::vector<std::optional<double>> prices(nm);
    for (std::size_t i = 0; i < nm; ++i) {
        prices[i] = months[i].price;
    }
    for (std::size_t c = 0; c < n; ++c) {
        const std::size_t i = unknowns[c];
        if (!std::isfinite(x[c])) {
            auto r = refusalOf(CurveBuildRefusal::Reason::NotFinite);
            r.path = "contract.months[" + std::to_string(i) + "]";
            r.month = months[i].month;
            refuse(
                "contract.months[" + std::to_string(i) + "] (" + months[i].month +
                    "): the solved price is not finite",
                r);
        }
        prices[i] = x[c];
    }
    const FuturesContract solved = contract.withPrices(prices);
    const auto filled =
        BuildFuturesCurve(solved, inputs.interpolation, inputs.extrapolateFlat, asOf);
    result.months = filled.months;

    // Sensitivities: one solve per unit right-hand side.
    std::vector<Vector> sens; // sens[e][c] = d x_c / d quote e
    if (inputs.includeDetails) {
        for (std::size_t e = 0; e < rows.size(); ++e) {
            Vector unit(rows.size(), 0.0);
            unit[e] = 1.0;
            sens.push_back(solve(unit));
        }
        double largest = 0.0;
        for (const auto& v : sens) {
            for (const double s : v) {
                largest = std::max(largest, std::fabs(s));
            }
        }
        report.maxSensitivity = largest;
    }

    for (const std::size_t i : live) {
        MonthReport mr;
        mr.month = months[i].month;
        mr.price = *filled.months[i].price;
        const auto it = column.find(i);
        if (it != column.end()) {
            const std::size_t c = it->second;
            mr.source = months[i].price ? MonthSource::Quoted
                        : determined[c] ? MonthSource::Solved
                                        : MonthSource::Assumed;
            if (inputs.includeDetails) {
                for (std::size_t e = 0; e < rows.size(); ++e) {
                    QuoteSensitivity qs;
                    if (rowQuote[e]) {
                        qs.averageQuote = *rowQuote[e];
                    } else {
                        qs.month = rowNames[e];
                    }
                    qs.value = sens[e][c];
                    mr.sensitivities.push_back(qs);
                }
            }
        } else {
            const bool flat =
                std::find(
                    filled.extrapolatedMonths.begin(), filled.extrapolatedMonths.end(),
                    months[i].month) != filled.extrapolatedMonths.end();
            mr.source = flat ? MonthSource::Extrapolated : MonthSource::Interpolated;
        }
        report.months.push_back(std::move(mr));
    }

    // Every quote repriced by the swap pricer on the built contract.
    const auto built = std::make_shared<const FuturesContract>(contract.withPrices([&] {
        std::vector<std::optional<double>> p(nm);
        for (std::size_t i = 0; i < nm; ++i) {
            p[i] = filled.months[i].price;
        }
        return p;
    }()));
    for (std::size_t k = 0; k < quotes.size(); ++k) {
        const auto r = priceQuote(quotes[k], k, built, quotes[k].price, asOf);
        auto& qr = report.averageQuotes[k];
        qr.repriced = r.fairFixedPrice.value_or(0.0);
        qr.difference = qr.repriced - quotes[k].price;
        report.maxRepricingDifference =
            std::max(report.maxRepricingDifference, std::fabs(qr.difference));
        if (inputs.includeDetails) {
            for (const auto& w : equations[k].weights) {
                qr.weights.push_back({w.first, w.second});
            }
            qr.constant = equations[k].constant;
        }
    }
    return result;
}

std::string QualifyCurveBuildMessage(const std::string& build, const std::string& message) {
    const bool fieldPath = message.rfind("contract", 0) == 0 ||
                           message.rfind("interpolation", 0) == 0 ||
                           message.rfind("extrapolate_flat", 0) == 0 ||
                           message.rfind("average_quotes", 0) == 0 || message.rfind("rule", 0) == 0;
    return build + (fieldPath ? "." : ": ") + message;
}

}} // namespace quantra::commodities
