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
#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

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
        case CurveBuildRefusal::Reason::DateOutsideRange: return "DateOutsideRange";
        case CurveBuildRefusal::Reason::NoPricedPoint: return "NoPricedPoint";
        case CurveBuildRefusal::Reason::UnknownBase: return "UnknownBase";
        case CurveBuildRefusal::Reason::BaseNotContract: return "BaseNotContract";
        case CurveBuildRefusal::Reason::BasisCycle: return "BasisCycle";
        case CurveBuildRefusal::Reason::BaseNotDetermined: return "BaseNotDetermined";
        case CurveBuildRefusal::Reason::BasisAndPrice: return "BasisAndPrice";
        case CurveBuildRefusal::Reason::BasisOnExpiredMonth: return "BasisOnExpiredMonth";
        case CurveBuildRefusal::Reason::BaseNotSpot: return "BaseNotSpot";
        case CurveBuildRefusal::Reason::MissingFixing: return "MissingFixing";
        case CurveBuildRefusal::Reason::MonthWithoutPrice: return "MonthWithoutPrice";
        case CurveBuildRefusal::Reason::MonthNotListed: return "MonthNotListed";
        case CurveBuildRefusal::Reason::NoPricingDay: return "NoPricingDay";
        case CurveBuildRefusal::Reason::UnknownDiscountingCurve: return "UnknownDiscountingCurve";
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
/// receives the average of `floating` (a contract month or a spot price).
/// Value rules on the quote are checked here.
std::pair<CommoditySwapLeg, CommoditySwapLeg> quoteLegs(
    const AverageQuote& q,
    std::size_t k,
    const LegPrice& floating,
    double price,
    const QuantLib::Date& asOf) {
    const std::string path = quotePath(k);
    ValidatePeriods(q.periods, path + ".periods");
    if (std::holds_alternative<FuturesFloatingPrice>(floating)) {
        ValidateNearbyAndRoll(q.nearby, q.rollDaysBeforeExpiry, path);
    }
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
    CommoditySwapLeg floatingLeg = fixed;
    floatingLeg.payReceive = PayReceive::Receive;
    floatingLeg.price = floating;
    return {fixed, floatingLeg};
}

CommoditySwapResult priceQuoteLegs(
    const AverageQuote& q,
    std::size_t k,
    const LegPrice& floating,
    double price,
    const QuantLib::Date& asOf) {
    const auto legs = quoteLegs(q, k, floating, price, asOf);
    try {
        return PriceCommoditySwap(legs.first, legs.second, asOf);
    } catch (const SpotDateOutsidePoints& e) {
        // A pricing day outside the spot's points: the swap's sentence, with
        // the quote and the date as data.
        CurveBuildRefusal r;
        r.reason = CurveBuildRefusal::Reason::DateOutsideRange;
        const auto& d = e.data();
        r.path = d.path.rfind("leg", 0) == 0 && d.path.size() > 5
                     ? quotePath(k) + "." + d.path.substr(5)
                     : quotePath(k);
        r.averageQuote = k;
        r.date = DateToIso(e.date());
        refuse(quoteMessage(k, e.what()), r);
    } catch (const CommodityReadError& e) {
        // The swap pricer's refusal about the quote's days, with its data.
        const auto& d = e.data();
        using Reason = CurveBuildRefusal::Reason;
        const Reason reason = d.reason == "MissingFixing"       ? Reason::MissingFixing
                              : d.reason == "MonthWithoutPrice" ? Reason::MonthWithoutPrice
                              : d.reason == "MonthNotListed"    ? Reason::MonthNotListed
                              : d.reason == "NoPricingDay"      ? Reason::NoPricingDay
                                                                : Reason::InvalidInput;
        auto r = refusalOf(reason);
        // `leg2.periods[3]` is the quote's own `periods[3]`.
        r.path = d.path.rfind("leg", 0) == 0 && d.path.size() > 5
                     ? quotePath(k) + "." + d.path.substr(5)
                     : quotePath(k);
        r.averageQuote = k;
        r.date = d.date;
        r.month = d.month;
        refuse(quoteMessage(k, e.what()), r);
    } catch (const QuantraInvalidArgument& e) {
        QUANTRA_INVALID_ARGUMENT(quoteMessage(k, e.what()));
    }
    return {}; // unreachable
}

CommoditySwapResult priceQuote(
    const AverageQuote& q,
    std::size_t k,
    const std::shared_ptr<const FuturesContract>& contract,
    double price,
    const QuantLib::Date& asOf) {
    FuturesFloatingPrice ff;
    ff.contract = contract;
    ff.nearby = q.nearby;
    ff.rollDaysBeforeExpiry = q.rollDaysBeforeExpiry;
    return priceQuoteLegs(q, k, ff, price, asOf);
}

CommoditySwapResult priceQuote(
    const AverageQuote& q,
    std::size_t k,
    const std::shared_ptr<const SpotPrice>& spot,
    double price,
    const QuantLib::Date& asOf) {
    SpotFloatingPrice sf;
    sf.spot = spot;
    return priceQuoteLegs(q, k, sf, price, asOf);
}

/// One average quote as an equation: quote = constant + sum weight x price.
struct QuoteEquation {
    std::map<std::string, double> weights; // by month, or by date for a spot
    double constant = 0.0;
};

template <typename Source>
QuoteEquation quoteEquation(
    const AverageQuote& q,
    std::size_t k,
    const std::shared_ptr<const Source>& placeholder,
    const QuantLib::Date& asOf) {
    // Fixed price 0 and every live month (spot point) at 0: the NPV is the
    // realised part.
    const auto r = priceQuote(q, k, placeholder, 0.0, asOf);
    double annuity = 0.0;
    for (const auto& flow : r.legs[0].flows) {
        annuity += flow.quantity * flow.discount;
    }
    QuoteEquation eq;
    if (annuity > 0.0) {
        for (const auto& d : r.deltas) {
            if (d.delta != 0.0) {
                // Futures: by month; spot: by the point's date (YYYY-MM-DD,
                // which sorts in date order).
                const std::string key =
                    d.kind == CommodityPriceDelta::Kind::Futures ? d.month : DateToIso(d.date);
                eq.weights[key] = d.delta / annuity;
            }
        }
    }
    if (eq.weights.empty()) {
        auto r = refusalOf(CurveBuildRefusal::Reason::QuoteHasNoLiveMonth);
        r.path = quotePath(k);
        r.averageQuote = k;
        const std::string what = std::is_same<Source, SpotPrice>::value ? "point" : "month";
        refuse(
            quotePath(k) + ": no live " + what +
                ": every pricing day it averages is a fixing or every period is "
                "already paid, so it constrains no price",
            r);
    }
    eq.constant = r.legs[1].npv / annuity;
    return eq;
}

/// Spread quotes (versus_base): the base side's fair price of each quote, 0
/// for the others. The rules of the quote's base fields are checked here.
Vector baseSides(const CurveBuildInputs& inputs, bool spot, const QuantLib::Date& asOf) {
    const auto& quotes = inputs.averageQuotes;
    Vector out(quotes.size(), 0.0);
    for (std::size_t k = 0; k < quotes.size(); ++k) {
        const auto& q = quotes[k];
        const std::string path = quotePath(k);
        auto invalid = [&](const std::string& field, const std::string& message) {
            auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
            r.path = path + "." + field;
            r.averageQuote = k;
            refuse(path + "." + field + " " + message, r);
        };
        if (!q.versusBase) {
            if (q.baseNearby) {
                invalid("base_nearby", "is only used with versus_base");
            }
            if (q.baseRollDaysBeforeExpiry) {
                invalid("base_roll_days_before_expiry", "is only used with versus_base");
            }
            continue;
        }
        if (!inputs.basis) {
            invalid(
                "versus_base",
                "needs basis: a spread to the base is quoted on a build that has a base");
        }
        CommoditySwapResult r;
        if (spot) {
            if (q.baseNearby) {
                invalid("base_nearby", "is not used on a spot price build");
            }
            if (q.baseRollDaysBeforeExpiry) {
                invalid("base_roll_days_before_expiry", "is not used on a spot price build");
            }
            r = priceQuote(q, k, inputs.basis->baseSpot, 0.0, asOf);
        } else {
            if (!q.baseNearby) {
                invalid(
                    "base_nearby",
                    "is required with versus_base on a contract build (the base month averaged)");
            }
            if (!q.baseRollDaysBeforeExpiry) {
                invalid(
                    "base_roll_days_before_expiry",
                    "is required with versus_base on a contract build");
            }
            if (*q.baseNearby < 1) {
                invalid("base_nearby", "must be >= 1 (1 = front month)");
            }
            if (*q.baseRollDaysBeforeExpiry < 0) {
                invalid("base_roll_days_before_expiry", "must be >= 0");
            }
            AverageQuote onBase = q;
            onBase.nearby = *q.baseNearby;
            onBase.rollDaysBeforeExpiry = *q.baseRollDaysBeforeExpiry;
            r = priceQuote(onBase, k, inputs.basis->base, 0.0, asOf);
        }
        out[k] = r.fairFixedPrice.value_or(0.0);
    }
    return out;
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

/// Second divided differences over n unknowns at the abscissas x
/// ((n-2) x n): (p[i+1] - p[i]) / (x[i+1] - x[i]) - (p[i] - p[i-1]) / (x[i] -
/// x[i-1]), divided by x[i+1] - x[i-1].
std::vector<Vector> secondDividedDifferences(const Vector& x) {
    const std::size_t n = x.size();
    std::vector<Vector> d;
    for (std::size_t c = 1; c + 1 < n; ++c) {
        const double h0 = x[c] - x[c - 1];
        const double h1 = x[c + 1] - x[c];
        const double span = x[c + 1] - x[c - 1];
        Vector row(n, 0.0);
        row[c - 1] = 1.0 / (h0 * span);
        row[c] = -(1.0 / h0 + 1.0 / h1) / span;
        row[c + 1] = 1.0 / (h1 * span);
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

/// The linear system of a build: one row per equation (direct prices first,
/// then the average quotes), in the unknowns' order.
struct SystemInputs {
    std::vector<Vector> rows;
    Vector rhs;
    /// The quoted value of each row (the tolerance scale).
    Vector quoted;
    /// For messages: a direct price's label (month or date) or the quote's path.
    std::vector<std::string> rowNames;
    std::vector<std::optional<std::size_t>> rowQuote;
    /// The unknowns' labels (months or dates), in order.
    std::vector<std::string> labels;
    /// Labels are dates (a spot price) rather than months.
    bool byDate = false;
    /// The unknowns' noun in messages ("months", "points").
    std::string noun;
    /// Smooth's penalty rows.
    std::vector<Vector> secondDifferences;
    std::optional<CurveRule> rule;
    /// Per average quote: its equation's constant and its quoted price.
    Vector constants;
    Vector quotePrices;
};

struct SystemSolution {
    std::vector<bool> determined;
    /// The solve as a linear function of the right-hand sides.
    std::function<Vector(const Vector&)> solve;
    std::shared_ptr<EquationSpace> space;
};

void setLabel(CurveBuildRefusal& r, bool byDate, const std::string& label) {
    (byDate ? r.date : r.month) = label;
}

void setLabels(CurveBuildRefusal& r, bool byDate, const std::vector<std::string>& labels) {
    (byDate ? r.dates : r.months) = labels;
}

/// Solve: conflicts and redundant quotes, status, rank, rule. Fills the
/// report's status, rank, unknowns, equations and quote statuses.
SystemSolution SolveSystem(const SystemInputs& in, CurveBuildReport& report) {
    const auto& rows = in.rows;
    const auto& rhs = in.rhs;
    const auto& quoted = in.quoted;
    const auto& rowNames = in.rowNames;
    const auto& rowQuote = in.rowQuote;
    const std::size_t n = in.labels.size();
    report.unknowns = static_cast<int>(n);
    report.equations = static_cast<int>(rows.size());
    report.rule = in.rule;
    report.averageQuotes.resize(in.quotePrices.size());

    SystemSolution out;
    out.space = std::make_shared<EquationSpace>(n);
    auto& space = *out.space;
    for (std::size_t e = 0; e < rows.size(); ++e) {
        const auto added = space.add(rows[e], rhs[e]);
        if (added.independent) {
            continue;
        }
        const std::size_t k = *rowQuote[e]; // direct prices are never dependent
        const double implied = added.implied + in.constants[k];
        const double gap = in.quotePrices[k] - implied;
        if (std::fabs(gap) <= 1e-8 * std::max(1.0, std::fabs(in.quotePrices[k]))) {
            report.averageQuotes[k].status = QuoteStatus::Redundant;
            continue;
        }
        std::vector<std::string> involved;
        auto r = refusalOf(CurveBuildRefusal::Reason::QuoteConflict);
        r.path = quotePath(k);
        r.averageQuote = k;
        r.impliedValue = implied;
        r.quotedValue = in.quotePrices[k];
        r.gap = gap;
        for (std::size_t j = 0; j < added.combination.size(); ++j) {
            if (std::fabs(added.combination[j]) > 1e-12) {
                involved.push_back(rowNames[j]);
                CurveBuildRefusal::Input input;
                if (rowQuote[j]) {
                    input.averageQuote = *rowQuote[j];
                } else if (in.byDate) {
                    input.date = rowNames[j];
                } else {
                    input.month = rowNames[j];
                }
                r.dependsOn.push_back(input);
            }
        }
        refuse(
            quotePath(k) + ": conflicts with the other quotes: they imply " +
                formatNumber(implied) + ", it quotes " + formatNumber(in.quotePrices[k]) +
                ", a gap of " + formatNumber(gap) + " (it is determined by " + joinNames(involved) +
                ")",
            r);
    }
    report.rank = static_cast<int>(space.rank());

    auto& determined = out.determined;
    determined.assign(n, false);
    std::vector<std::string> undetermined;
    for (std::size_t c = 0; c < n; ++c) {
        Vector e(n, 0.0);
        e[c] = 1.0;
        determined[c] = space.spans(e);
        if (!determined[c]) {
            undetermined.push_back(in.labels[c]);
        }
    }

    auto& solve = out.solve;
    const auto spacePtr = out.space;
    if (space.rank() == n) {
        report.status = CurveStatus::Unique;
        solve = [spacePtr](const Vector& b) { return spacePtr->solve(b); };
    } else {
        report.status = CurveStatus::Underdetermined;
        if (!in.rule) {
            auto r = refusalOf(CurveBuildRefusal::Reason::RuleRequired);
            r.path = "rule";
            setLabels(r, in.byDate, undetermined);
            refuse(
                "rule is required: the quotes do not determine " + joinNames(undetermined) + " (" +
                    std::to_string(n) + " " + in.noun + ", " + std::to_string(space.rank()) +
                    " independent quotes); give Flat or Smooth",
                r);
        }
        if (*in.rule == CurveRule::Flat) {
            // Unknowns touched by the same equations share one price.
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
                    setLabels(r, in.byDate, undetermined);
                    if (rowQuote[e]) {
                        r.averageQuote = *rowQuote[e];
                    } else {
                        setLabel(r, in.byDate, rowNames[e]);
                    }
                    r.gap = rhs[e] - added.implied;
                    refuse(
                        "rule Flat cannot resolve these quotes: with one price per group of " +
                            in.noun + " touched by the same quotes, " + rowNames[e] +
                            " is no longer repriced (gap " + formatNumber(rhs[e] - added.implied) +
                            ")",
                        r);
                }
            }
            if (reduced->rank() < g) {
                auto r = refusalOf(CurveBuildRefusal::Reason::FlatCannotResolve);
                r.path = "rule";
                r.rule = CurveRule::Flat;
                setLabels(r, in.byDate, undetermined);
                refuse(
                    "rule Flat cannot resolve these quotes: one price per group of " + in.noun +
                        " touched by the same quotes still leaves " + joinNames(undetermined) +
                        " undetermined",
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
            const auto& d = in.secondDifferences;
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
                setLabels(r, in.byDate, undetermined);
                refuse(
                    "rule Smooth has no unique solution: the quotes leave " +
                        joinNames(undetermined) +
                        " free in a way the second differences do not fix (they need a level "
                        "and a slope); use Flat",
                    r);
            }
            solve = [spacePtr, basis, d, m, h, n](const Vector& b) {
                Vector x = spacePtr->solve(b);
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
    return out;
}

} // namespace

namespace {

/// "YYYY-MM" moved back `n` months.
std::string monthsBefore(const std::string& month, int n) {
    int y = std::stoi(month.substr(0, 4));
    int m = std::stoi(month.substr(5, 2)) - n;
    while (m < 1) {
        m += 12;
        --y;
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04d-%02d", y, m);
    return buf;
}

[[noreturn]] void baseNotDetermined(
    const BasisInputs& b,
    const std::string& month,
    const std::string& baseMonth,
    const std::string& why) {
    auto r = refusalOf(CurveBuildRefusal::Reason::BaseNotDetermined);
    r.path = "basis.base";
    r.month = month;
    r.baseCurve = b.baseCurve;
    r.baseMonth = baseMonth;
    refuse(
        "basis.base: no base price for " + month + ": the base contract '" + b.base->id() + "' " +
            why,
        r);
}

/// The base price of `month` (rules in the header).
double basePriceOf(const BasisInputs& b, const ContractMonth& month, const QuantLib::Date& asOf) {
    const auto& base = *b.base;
    if (b.basePrice == BasisBasePrice::SameMonth) {
        const std::string baseMonth = monthsBefore(month.month, b.baseMonthOffset);
        for (const auto& m : base.months()) {
            if (m.month != baseMonth) {
                continue;
            }
            if (m.lastTradeDate >= asOf) {
                if (!m.price) {
                    baseNotDetermined(b, month.month, baseMonth, "has no price for " + baseMonth);
                }
                return *m.price;
            }
            const auto fixing = base.fixing(baseMonth, m.lastTradeDate);
            if (!fixing) {
                baseNotDetermined(
                    b, month.month, baseMonth,
                    "month " + baseMonth + " expired on " + DateToIso(m.lastTradeDate) +
                        " and has no fixing on that day");
            }
            return *fixing;
        }
        baseNotDetermined(b, month.month, baseMonth, "does not list " + baseMonth);
    }
    // MonthAverage: the swap pricer's average of the base over calendar
    // month M, one period, undiscounted.
    const int y = std::stoi(month.month.substr(0, 4));
    const int mm = std::stoi(month.month.substr(5, 2));
    const QuantLib::Date first(1, static_cast<QuantLib::Month>(mm), y);
    AverageQuote q;
    q.periods = {{first, QuantLib::Date::endOfMonth(first)}};
    q.nearby = *b.baseNearby;
    q.rollDaysBeforeExpiry = *b.baseRollDaysBeforeExpiry;
    FuturesFloatingPrice ff;
    ff.contract = b.base;
    ff.nearby = q.nearby;
    ff.rollDaysBeforeExpiry = q.rollDaysBeforeExpiry;
    CommoditySwapResult r;
    try {
        const auto legs = quoteLegs(q, 0, ff, 0.0, asOf);
        r = PriceCommoditySwap(legs.first, legs.second, asOf);
    } catch (const QuantraInvalidArgument& e) {
        baseNotDetermined(
            b, month.month, month.month,
            std::string("cannot be averaged over ") + month.month + ": " + e.what());
    }
    if (r.legs[1].flows.empty() || !r.legs[1].flows[0].average) {
        baseNotDetermined(
            b, month.month, month.month, "has no pricing day left to average over " + month.month);
    }
    return *r.legs[1].flows[0].average;
}

} // namespace

CommodityCurveBuildResult BuildCommodityCurve(
    const FuturesContract& input, const CurveBuildInputs& inputs, const QuantLib::Date& asOf) {
    // Basis: every live month's base price, then the basis quotes as direct
    // prices.
    std::map<std::size_t, double> baseOf;
    std::set<std::size_t> basisMonths;
    std::optional<FuturesContract> withBasis;
    if (inputs.basis) {
        const auto& b = *inputs.basis;
        if (b.baseMonthOffset < 0) {
            auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
            r.path = "basis.base_month_offset";
            refuse("basis.base_month_offset must be >= 0", r);
        }
        const bool average = b.basePrice == BasisBasePrice::MonthAverage;
        for (const auto& [given, field] :
             {std::make_pair(b.baseNearby.has_value(), std::string("base_nearby")),
              std::make_pair(
                  b.baseRollDaysBeforeExpiry.has_value(),
                  std::string("base_roll_days_before_expiry"))}) {
            if (average && !given) {
                auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
                r.path = "basis." + field;
                refuse("basis." + field + " is required when base_price is MonthAverage", r);
            }
            if (!average && given) {
                auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
                r.path = "basis." + field;
                refuse(
                    "basis." + field +
                        " is not used when base_price is SameMonth (it reads one base month)",
                    r);
            }
        }
        if (average && (*b.baseNearby < 1 || *b.baseRollDaysBeforeExpiry < 0)) {
            const bool nearby = *b.baseNearby < 1;
            auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
            r.path = nearby ? "basis.base_nearby" : "basis.base_roll_days_before_expiry";
            refuse(r.path + (nearby ? " must be >= 1 (1 = front month)" : " must be >= 0"), r);
        }
        if (b.baseMonthOffset != 0 && average) {
            auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
            r.path = "basis.base_month_offset";
            refuse("basis.base_month_offset is not used when base_price is MonthAverage", r);
        }
        const auto& months = input.months();
        std::map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < months.size(); ++i) {
            index[months[i].month] = i;
        }
        std::vector<std::optional<double>> prices(months.size());
        for (std::size_t i = 0; i < months.size(); ++i) {
            prices[i] = months[i].price;
        }
        for (std::size_t k = 0; k < b.quotes.size(); ++k) {
            const auto& quote = b.quotes[k];
            const std::string path = "basis.quotes[" + std::to_string(k) + "]";
            const auto it = index.find(quote.month);
            if (it == index.end()) {
                auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
                r.path = path + ".month";
                r.month = quote.month;
                refuse(path + ".month " + quote.month + " is not a month of the contract", r);
            }
            const std::size_t i = it->second;
            if (!std::isfinite(quote.value)) {
                auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
                r.path = path + ".value";
                refuse(path + ".value must be finite", r);
            }
            if (basisMonths.count(i) > 0) {
                auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
                r.path = path + ".month";
                r.month = quote.month;
                refuse(path + ".month " + quote.month + " has a second basis quote", r);
            }
            if (months[i].price) {
                auto r = refusalOf(CurveBuildRefusal::Reason::BasisAndPrice);
                r.path = path;
                r.month = quote.month;
                refuse(
                    path + ": month " + quote.month +
                        " also has a price; give either the price or the basis",
                    r);
            }
            if (months[i].lastTradeDate < asOf) {
                auto r = refusalOf(CurveBuildRefusal::Reason::BasisOnExpiredMonth);
                r.path = path;
                r.month = quote.month;
                refuse(
                    path + ": month " + quote.month + " expired on " +
                        DateToIso(months[i].lastTradeDate) +
                        " (before the valuation date); it is read through fixings",
                    r);
            }
            basisMonths.insert(i);
        }
        for (std::size_t i = 0; i < months.size(); ++i) {
            if (months[i].lastTradeDate >= asOf) {
                baseOf[i] = basePriceOf(b, months[i], asOf);
            }
        }
        for (std::size_t k = 0; k < b.quotes.size(); ++k) {
            const std::size_t i = index[b.quotes[k].month];
            prices[i] = baseOf[i] + b.quotes[k].value;
        }
        withBasis.emplace(input.withPrices(prices));
    }
    const FuturesContract& contract = withBasis ? *withBasis : input;
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

    // Spread quotes: the base side is a constant.
    const Vector baseSide = baseSides(inputs, false, asOf);
    for (std::size_t k = 0; k < quotes.size(); ++k) {
        equations[k].constant -= baseSide[k];
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
    std::vector<std::string> labels;
    for (const std::size_t i : unknowns) {
        labels.push_back(months[i].month);
    }
    // Smooth: second divided differences on the last trading dates.
    Vector abscissas;
    for (const std::size_t i : unknowns) {
        abscissas.push_back(static_cast<double>(months[i].lastTradeDate - asOf) / 365.0);
    }
    SystemInputs system{rows,       rhs,      quoted,
                        rowNames,   rowQuote, labels,
                        false,      "months", secondDividedDifferences(abscissas),
                        inputs.rule};
    for (std::size_t k = 0; k < quotes.size(); ++k) {
        system.constants.push_back(equations[k].constant);
        system.quotePrices.push_back(quotes[k].price);
    }
    const auto solution = SolveSystem(system, report);
    const auto& determined = solution.determined;
    const auto& solve = solution.solve;
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
    FuturesCurveBuildResult filled;
    if (!inputs.basis) {
        filled = BuildFuturesCurve(solved, inputs.interpolation, inputs.extrapolateFlat, asOf);
    } else {
        // The fill reads the basis of the determined months, then adds the
        // base back.
        std::vector<std::optional<double>> basisPrices(nm);
        for (std::size_t i = 0; i < nm; ++i) {
            basisPrices[i] = prices[i];
            const auto it = baseOf.find(i);
            if (it != baseOf.end() && prices[i]) {
                basisPrices[i] = *prices[i] - it->second;
            }
        }
        try {
            filled = BuildFuturesCurve(
                contract.withPrices(basisPrices), inputs.interpolation, inputs.extrapolateFlat,
                asOf);
        } catch (const CurveBuildError& e) {
            if (e.refusal().reason != CurveBuildRefusal::Reason::NonPositivePrice) {
                throw;
            }
            // Worded for the basis.
            const auto& r = e.refusal();
            const std::size_t i = monthIndex[*r.month];
            refuse(
                "contract.months[" + std::to_string(i) + "] (" + *r.month + "): basis " +
                    formatPrice(*basisPrices[i]) +
                    " must be > 0 for interpolation LogLinear (it reads log basis)",
                r);
        }
        for (std::size_t i = 0; i < nm; ++i) {
            const auto it = baseOf.find(i);
            if (it != baseOf.end()) {
                filled.months[i].price = *filled.months[i].price + it->second;
            }
        }
    }
    result.months = filled.months;

    // Sensitivities: one solve per unit right-hand side.
    std::vector<Vector> sens; // sens[e][c] = d x_c / d quote e
    if (inputs.includeDetails || inputs.fullJacobian) {
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
        if (inputs.includeDetails) {
            report.maxSensitivity = largest;
        }
    }

    for (const std::size_t i : live) {
        MonthReport mr;
        mr.month = months[i].month;
        mr.price = *filled.months[i].price;
        if (inputs.basis) {
            mr.basePrice = baseOf.at(i);
            mr.basis = mr.price - *mr.basePrice;
        }
        const auto it = column.find(i);
        if (it != column.end()) {
            const std::size_t c = it->second;
            mr.source = basisMonths.count(i) > 0 ? MonthSource::Basis
                        : months[i].price        ? MonthSource::Quoted
                        : determined[c]          ? MonthSource::Solved
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

    if (inputs.fullJacobian) {
        auto& jac = result.jacobian;
        for (std::size_t e = 0; e < rows.size(); ++e) {
            QuoteSensitivity qs;
            if (rowQuote[e]) {
                qs.averageQuote = *rowQuote[e];
            } else {
                qs.month = rowNames[e];
            }
            jac.inputs.push_back(qs);
        }
        // The fill's nodes: the determined months' prices, or their basis.
        std::vector<std::optional<double>> nodes(nm);
        for (std::size_t i = 0; i < nm; ++i) {
            nodes[i] = prices[i];
            const auto it = baseOf.find(i);
            if (it != baseOf.end() && prices[i]) {
                nodes[i] = *prices[i] - it->second;
            }
        }
        // d filled month / d node, by central differences on the fill.
        std::map<std::size_t, Vector> dfill;
        for (const std::size_t i : live) {
            if (column.count(i) == 0) {
                dfill[i] = Vector(n, 0.0);
            }
        }
        if (!dfill.empty()) {
            for (std::size_t c = 0; c < n; ++c) {
                const std::size_t i = unknowns[c];
                const double h = 1e-6 * std::max(1.0, std::fabs(*nodes[i]));
                auto up = nodes;
                auto down = nodes;
                *up[i] += h;
                *down[i] -= h;
                const auto mu =
                    BuildFuturesCurve(
                        contract.withPrices(up), inputs.interpolation, inputs.extrapolateFlat, asOf)
                        .months;
                const auto md = BuildFuturesCurve(
                                    contract.withPrices(down), inputs.interpolation,
                                    inputs.extrapolateFlat, asOf)
                                    .months;
                for (auto& [m, row] : dfill) {
                    row[c] = (*mu[m].price - *md[m].price) / (2.0 * h);
                }
            }
        }
        for (const std::size_t i : live) {
            Vector row(rows.size(), 0.0);
            const auto it = column.find(i);
            for (std::size_t e = 0; e < rows.size(); ++e) {
                if (it != column.end()) {
                    row[e] = sens[e][it->second];
                } else {
                    for (std::size_t c = 0; c < n; ++c) {
                        row[e] += dfill[i][c] * sens[e][c];
                    }
                }
            }
            jac.rows.push_back(std::move(row));
        }
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
        qr.repriced = r.fairFixedPrice.value_or(0.0) - baseSide[k];
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

SpotCurveBuildResult BuildSpotCurve(
    const SpotCurveSpec& spec, const CurveBuildInputs& inputs, const QuantLib::Date& asOf) {
    // A working copy: basis quotes become the points' direct prices.
    std::vector<SpotNode> points = spec.points;
    const std::size_t np = points.size();
    const auto& quotes = inputs.averageQuotes;

    // The spot's own rules (at least one point, dates ascending, finite
    // prices and fixings) through its constructor, every unpriced point at 0.
    std::vector<SpotPoint> zero;
    zero.reserve(np);
    for (const auto& p : points) {
        zero.push_back({p.date, p.price.value_or(0.0)});
    }
    std::shared_ptr<const SpotPrice> placeholder;
    try {
        placeholder = std::make_shared<const SpotPrice>(
            spec.id, spec.calendar, spec.interpolation, spec.extrapolateFlat, zero, spec.fixings);
    } catch (const QuantraInvalidArgument& e) {
        auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
        r.path = e.path().empty() ? "spot" : "spot." + e.path();
        refuse(std::string("spot.") + e.what(), r);
    }
    for (std::size_t k = 0; k < quotes.size(); ++k) {
        if (!std::isfinite(quotes[k].price)) {
            auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
            r.path = quotePath(k) + ".price";
            r.averageQuote = k;
            refuse(quotePath(k) + ".price must be finite", r);
        }
    }
    // Basis: every point's base price (the base read on its date), then the
    // basis quotes as direct prices.
    std::vector<double> baseAt;
    std::set<std::size_t> basisPoints;
    if (inputs.basis) {
        const auto& b = *inputs.basis;
        const auto& base = *b.baseSpot;
        baseAt.resize(np);
        for (std::size_t i = 0; i < np; ++i) {
            try {
                baseAt[i] = base.forward(points[i].date);
            } catch (const SpotDateOutsidePoints& e) {
                auto r = refusalOf(CurveBuildRefusal::Reason::BaseNotDetermined);
                r.path = "basis.base";
                r.date = DateToIso(points[i].date);
                r.baseCurve = b.baseCurve;
                refuse(
                    "basis.base: no base price for " + DateToIso(points[i].date) +
                        ": the base spot price '" + base.id() +
                        "' cannot be read there: " + e.what(),
                    r);
            }
        }
        std::map<QuantLib::Date, std::size_t> at;
        for (std::size_t i = 0; i < np; ++i) {
            at[points[i].date] = i;
        }
        for (std::size_t k = 0; k < b.quotes.size(); ++k) {
            const auto& quote = b.quotes[k];
            const std::string path = "basis.quotes[" + std::to_string(k) + "]";
            auto invalid = [&](const std::string& field, const std::string& message) {
                auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
                r.path = path + field;
                if (quote.date) {
                    r.date = DateToIso(*quote.date);
                }
                refuse(path + field + " " + message, r);
            };
            if (!quote.date) {
                invalid(".date", "is required on a spot price build");
            }
            const std::string iso = DateToIso(*quote.date);
            const auto it = at.find(*quote.date);
            if (it == at.end()) {
                invalid(".date", iso + " is not the date of a point of the spot price");
            }
            const std::size_t i = it->second;
            if (!std::isfinite(quote.value)) {
                invalid(".value", "must be finite");
            }
            if (basisPoints.count(i) > 0) {
                invalid(".date", iso + " has a second basis quote");
            }
            if (points[i].price) {
                auto r = refusalOf(CurveBuildRefusal::Reason::BasisAndPrice);
                r.path = path;
                r.date = iso;
                refuse(
                    path + ": point " + iso +
                        " also has a price; give either the price or the basis",
                    r);
            }
            basisPoints.insert(i);
        }
        for (const auto& quote : b.quotes) {
            const std::size_t i = at[*quote.date];
            points[i].price = baseAt[i] + quote.value;
        }
    }

    // Every point at 0 (also the priced ones): the weights do not depend on
    // the prices, and the constant is the realised part only.
    for (auto& p : zero) {
        p.price = 0.0;
    }
    const auto zeroSpot = std::make_shared<const SpotPrice>(
        spec.id, spec.calendar, spec.interpolation, spec.extrapolateFlat, zero, spec.fixings);

    std::vector<QuoteEquation> equations;
    equations.reserve(quotes.size());
    for (std::size_t k = 0; k < quotes.size(); ++k) {
        try {
            equations.push_back(quoteEquation(quotes[k], k, zeroSpot, asOf));
        } catch (const CurveBuildError&) {
            throw;
        } catch (const QuantraInvalidArgument& e) {
            if (e.path().empty()) {
                throw;
            }
            auto r = refusalOf(CurveBuildRefusal::Reason::InvalidInput);
            r.path = e.path();
            r.averageQuote = k;
            refuse(e.what(), r);
        }
    }

    // Spread quotes: the base side is a constant.
    const Vector baseSide = baseSides(inputs, true, asOf);
    for (std::size_t k = 0; k < quotes.size(); ++k) {
        equations[k].constant -= baseSide[k];
    }

    // Unknowns: points with a price or a weight, in date order.
    std::map<std::string, std::size_t> pointIndex;
    std::vector<std::string> isoDates(np);
    for (std::size_t i = 0; i < np; ++i) {
        isoDates[i] = DateToIso(points[i].date);
        pointIndex[isoDates[i]] = i;
    }
    std::vector<std::size_t> unknowns;
    std::map<std::size_t, std::size_t> column;
    for (std::size_t i = 0; i < np; ++i) {
        bool touched = points[i].price.has_value();
        for (const auto& eq : equations) {
            touched = touched || eq.weights.count(isoDates[i]) > 0;
        }
        if (touched) {
            column[i] = unknowns.size();
            unknowns.push_back(i);
        }
    }
    const std::size_t n = unknowns.size();

    std::vector<Vector> rows;
    Vector rhs;
    Vector quoted;
    std::vector<std::string> rowNames;
    std::vector<std::optional<std::size_t>> rowQuote;
    for (const std::size_t i : unknowns) {
        if (points[i].price) {
            Vector row(n, 0.0);
            row[column[i]] = 1.0;
            rows.push_back(row);
            rhs.push_back(*points[i].price);
            quoted.push_back(*points[i].price);
            rowNames.push_back(isoDates[i]);
            rowQuote.push_back(std::nullopt);
        }
    }
    for (std::size_t k = 0; k < quotes.size(); ++k) {
        Vector row(n, 0.0);
        for (const auto& w : equations[k].weights) {
            row[column[pointIndex[w.first]]] = w.second;
        }
        rows.push_back(row);
        rhs.push_back(quotes[k].price - equations[k].constant);
        quoted.push_back(quotes[k].price);
        rowNames.push_back(quotePath(k));
        rowQuote.push_back(k);
    }

    // Smooth: second divided differences on x = days from the valuation date
    // / 365 over the unknowns in date order.
    Vector pointAbscissas;
    for (const std::size_t i : unknowns) {
        pointAbscissas.push_back(static_cast<double>(points[i].date - asOf) / 365.0);
    }
    const std::vector<Vector> divided = secondDividedDifferences(pointAbscissas);

    SpotCurveBuildResult result;
    auto& report = result.report;
    std::vector<std::string> labels;
    for (const std::size_t i : unknowns) {
        labels.push_back(isoDates[i]);
    }
    SystemInputs system{rows,   rhs,  quoted,   rowNames, rowQuote,
                        labels, true, "points", divided,  inputs.rule};
    for (std::size_t k = 0; k < quotes.size(); ++k) {
        system.constants.push_back(equations[k].constant);
        system.quotePrices.push_back(quotes[k].price);
    }
    const auto solution = SolveSystem(system, report);
    const auto& determined = solution.determined;
    const auto& solve = solution.solve;
    const Vector x = solve(rhs);

    // The determined points, then the fill of the others with the spot's
    // own reading.
    std::vector<std::optional<double>> prices(np);
    for (std::size_t c = 0; c < n; ++c) {
        const std::size_t i = unknowns[c];
        if (!std::isfinite(x[c])) {
            auto r = refusalOf(CurveBuildRefusal::Reason::NotFinite);
            r.path = "spot.points[" + std::to_string(i) + "]";
            r.date = isoDates[i];
            refuse(
                "spot.points[" + std::to_string(i) + "] (" + isoDates[i] +
                    "): the solved price is not finite",
                r);
        }
        prices[i] = x[c];
    }
    std::vector<std::size_t> missing;
    std::vector<SpotPoint> nodes;
    for (std::size_t i = 0; i < np; ++i) {
        if (prices[i]) {
            nodes.push_back({points[i].date, *prices[i] - (baseAt.empty() ? 0.0 : baseAt[i])});
        } else {
            missing.push_back(i);
        }
    }
    std::vector<bool> outside(np, false);
    if (!missing.empty()) {
        std::vector<std::string> missingDates;
        for (const std::size_t i : missing) {
            missingDates.push_back(isoDates[i]);
        }
        if (nodes.empty()) {
            auto r = refusalOf(CurveBuildRefusal::Reason::NoPricedPoint);
            r.path = "spot.points";
            r.dates = missingDates;
            refuse(
                "spot.points: no point has a price or is read by a quote, so " +
                    joinNames(missingDates) + " cannot be filled",
                r);
        }
        const SpotPrice line(
            spec.id, spec.calendar, spec.interpolation, spec.extrapolateFlat, nodes, {});
        const QuantLib::Date first = nodes.front().date;
        const QuantLib::Date last = nodes.back().date;
        for (const std::size_t i : missing) {
            const QuantLib::Date date = points[i].date;
            outside[i] = date < first || date > last;
            if (outside[i] && !spec.extrapolateFlat) {
                auto r = refusalOf(CurveBuildRefusal::Reason::DateOutsideRange);
                r.path = "spot.points[" + std::to_string(i) + "]";
                r.date = isoDates[i];
                for (const std::size_t j : missing) {
                    if (points[j].date < first || points[j].date > last) {
                        r.dates.push_back(isoDates[j]);
                    }
                }
                refuse(
                    "spot.points[" + std::to_string(i) + "] (" + isoDates[i] + ") is " +
                        (date < first ? "before the first" : "after the last") +
                        " determined point (" + DateToIso(first) + " to " + DateToIso(last) +
                        ") and extrapolate_flat is false",
                    r);
            }
            const double price = line.forward(date);
            if (!std::isfinite(price)) {
                auto r = refusalOf(CurveBuildRefusal::Reason::NotFinite);
                r.path = "spot.points[" + std::to_string(i) + "]";
                r.date = isoDates[i];
                refuse(
                    "spot.points[" + std::to_string(i) + "] (" + isoDates[i] +
                        "): the filled price is not finite",
                    r);
            }
            prices[i] = price + (baseAt.empty() ? 0.0 : baseAt[i]);
        }
    }
    result.points.reserve(np);
    std::vector<SpotPoint> built;
    for (std::size_t i = 0; i < np; ++i) {
        result.points.push_back({points[i].date, prices[i]});
        built.push_back({points[i].date, *prices[i]});
    }

    std::vector<Vector> sens;
    if (inputs.includeDetails || inputs.fullJacobian) {
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
        if (inputs.includeDetails) {
            report.maxSensitivity = largest;
        }
    }
    for (std::size_t i = 0; i < np; ++i) {
        PointReport pr;
        pr.date = points[i].date;
        pr.price = *prices[i];
        if (!baseAt.empty()) {
            pr.basePrice = baseAt[i];
            pr.basis = pr.price - baseAt[i];
        }
        const auto it = column.find(i);
        if (it != column.end()) {
            const std::size_t c = it->second;
            pr.source = basisPoints.count(i) > 0 ? MonthSource::Basis
                        : points[i].price        ? MonthSource::Quoted
                        : determined[c]          ? MonthSource::Solved
                                                 : MonthSource::Assumed;
            if (inputs.includeDetails) {
                for (std::size_t e = 0; e < rows.size(); ++e) {
                    QuoteSensitivity qs;
                    if (rowQuote[e]) {
                        qs.averageQuote = *rowQuote[e];
                    } else {
                        qs.date = rowNames[e];
                    }
                    qs.value = sens[e][c];
                    pr.sensitivities.push_back(qs);
                }
            }
        } else {
            pr.source = outside[i] ? MonthSource::Extrapolated : MonthSource::Interpolated;
        }
        report.points.push_back(std::move(pr));
    }

    if (inputs.fullJacobian) {
        auto& jac = result.jacobian;
        for (std::size_t e = 0; e < rows.size(); ++e) {
            QuoteSensitivity qs;
            if (rowQuote[e]) {
                qs.averageQuote = *rowQuote[e];
            } else {
                qs.date = rowNames[e];
            }
            jac.inputs.push_back(qs);
        }
        // d filled point / d node (nodes = the determined points in date
        // order, their basis on a basis build), by central differences.
        std::map<std::size_t, Vector> dfill;
        for (const std::size_t i : missing) {
            dfill[i] = Vector(n, 0.0);
        }
        if (!dfill.empty()) {
            for (std::size_t c = 0; c < n; ++c) {
                const double h = 1e-6 * std::max(1.0, std::fabs(nodes[c].price));
                auto up = nodes;
                auto down = nodes;
                up[c].price += h;
                down[c].price -= h;
                const SpotPrice lu(
                    spec.id, spec.calendar, spec.interpolation, spec.extrapolateFlat, up, {});
                const SpotPrice ld(
                    spec.id, spec.calendar, spec.interpolation, spec.extrapolateFlat, down, {});
                for (auto& [i, row] : dfill) {
                    row[c] = (lu.forward(points[i].date) - ld.forward(points[i].date)) / (2.0 * h);
                }
            }
        }
        for (std::size_t i = 0; i < np; ++i) {
            Vector row(rows.size(), 0.0);
            const auto it = column.find(i);
            for (std::size_t e = 0; e < rows.size(); ++e) {
                if (it != column.end()) {
                    row[e] = sens[e][it->second];
                } else {
                    for (std::size_t c = 0; c < n; ++c) {
                        row[e] += dfill[i][c] * sens[e][c];
                    }
                }
            }
            jac.rows.push_back(std::move(row));
        }
    }

    // Every quote repriced by the swap pricer on the built spot price.
    const auto builtSpot = std::make_shared<const SpotPrice>(
        spec.id, spec.calendar, spec.interpolation, spec.extrapolateFlat, built, spec.fixings);
    for (std::size_t k = 0; k < quotes.size(); ++k) {
        const auto r = priceQuote(quotes[k], k, builtSpot, quotes[k].price, asOf);
        auto& qr = report.averageQuotes[k];
        qr.repriced = r.fairFixedPrice.value_or(0.0) - baseSide[k];
        qr.difference = qr.repriced - quotes[k].price;
        report.maxRepricingDifference =
            std::max(report.maxRepricingDifference, std::fabs(qr.difference));
        if (inputs.includeDetails) {
            for (const auto& w : equations[k].weights) {
                QuoteWeight qw;
                qw.date = w.first;
                qw.weight = w.second;
                qr.weights.push_back(qw);
            }
            qr.constant = equations[k].constant;
        }
    }
    return result;
}

std::string QualifyCurveBuildMessage(const std::string& build, const std::string& message) {
    const bool fieldPath =
        message.rfind("contract", 0) == 0 || message.rfind("interpolation", 0) == 0 ||
        message.rfind("extrapolate_flat", 0) == 0 || message.rfind("average_quotes", 0) == 0 ||
        message.rfind("rule", 0) == 0 || message.rfind("spot", 0) == 0 ||
        message.rfind("basis", 0) == 0;
    return build + (fieldPath ? "." : ": ") + message;
}

}} // namespace quantra::commodities
