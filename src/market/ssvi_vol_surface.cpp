#include "ssvi_vol_surface.h"

#include "error.h"

#include <ql/compounding.hpp>
#include <ql/math/comparison.hpp>
#include <ql/termstructures/yield/discountcurve.hpp>

#include <cmath>
#include <sstream>
#include <utility>

namespace quantra {

namespace {

ssvi::Surface buildSurface(
    const QuantLib::Date& referenceDate,
    const QuantLib::DayCounter& dayCounter,
    const std::vector<QuantLib::Date>& expiries,
    const std::vector<SsviSliceParams>& params) {
    if (expiries.size() != params.size()) {
        std::ostringstream os;
        os << "SSVI surface: " << expiries.size() << " expiries but " << params.size()
           << " parameter sets";
        QUANTRA_INVALID_ARGUMENT(os.str());
    }
    std::vector<ssvi::Slice> slices;
    slices.reserve(expiries.size());
    for (std::size_t i = 0; i < expiries.size(); ++i) {
        slices.push_back(
            ssvi::Slice{
                dayCounter.yearFraction(referenceDate, expiries[i]), params[i].theta, params[i].rho,
                params[i].psi});
    }
    return ssvi::Surface(std::move(slices));
}

// Instantaneous continuously compounded forward rate at t.
double instantaneousForward(const QuantLib::Handle<QuantLib::YieldTermStructure>& curve, double t) {
    return curve->forwardRate(t, t, QuantLib::Continuous, QuantLib::NoFrequency, true).rate();
}

double forwardOf(
    double spot,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& dividend,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFree,
    double t) {
    return spot * dividend->discount(t, true) / riskFree->discount(t, true);
}

} // namespace

QuantLib::Handle<QuantLib::YieldTermStructure> buildImpliedDividendCurve(
    const QuantLib::Date& referenceDate,
    const QuantLib::DayCounter& dayCounter,
    const std::vector<QuantLib::Date>& expiries,
    const std::vector<double>& forwards,
    double spot0,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFree) {
    if (expiries.empty() || expiries.size() != forwards.size()) {
        QUANTRA_INVALID_ARGUMENT("Implied dividend curve needs one forward per expiry");
    }
    if (!(spot0 > 0.0) || !std::isfinite(spot0) || riskFree.empty()) {
        QUANTRA_INVALID_ARGUMENT("Implied dividend curve needs a positive spot and a curve");
    }
    std::vector<QuantLib::Date> dates{referenceDate};
    std::vector<QuantLib::DiscountFactor> discounts{1.0};
    for (std::size_t i = 0; i < expiries.size(); ++i) {
        if (!(forwards[i] > 0.0) || !std::isfinite(forwards[i])) {
            std::ostringstream os;
            os << "Implied dividend curve: forward " << i << " must be finite and > 0, got "
               << forwards[i];
            QUANTRA_INVALID_ARGUMENT(os.str());
        }
        if (!(expiries[i] > dates.back())) {
            QUANTRA_INVALID_ARGUMENT(
                "Implied dividend curve: expiries must be strictly increasing after the reference "
                "date");
        }
        const double t = dayCounter.yearFraction(referenceDate, expiries[i]);
        dates.push_back(expiries[i]);
        discounts.push_back(forwards[i] * riskFree->discount(t, true) / spot0);
    }
    auto curve = std::make_shared<QuantLib::DiscountCurve>(dates, discounts, dayCounter);
    curve->enableExtrapolation();
    return QuantLib::Handle<QuantLib::YieldTermStructure>(curve);
}

SsviBlackVol::SsviBlackVol(
    const QuantLib::Date& referenceDate,
    const QuantLib::Calendar& calendar,
    QuantLib::BusinessDayConvention bdc,
    const QuantLib::DayCounter& dayCounter,
    const std::vector<QuantLib::Date>& expiries,
    const std::vector<SsviSliceParams>& params,
    double spot0,
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend,
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree)
    : QuantLib::BlackVarianceTermStructure(referenceDate, calendar, bdc, dayCounter),
      surface_(buildSurface(referenceDate, dayCounter, expiries, params)), spot0_(spot0),
      dividend_(std::move(dividend)), riskFree_(std::move(riskFree)) {
    if (!(spot0_ > 0.0) || !std::isfinite(spot0_)) {
        QUANTRA_INVALID_ARGUMENT("SSVI surface: spot0 must be finite and > 0");
    }
    if (dividend_.empty() || riskFree_.empty()) {
        QUANTRA_INVALID_ARGUMENT("SSVI surface: dividend and risk-free curves are required");
    }
    registerWith(dividend_);
    registerWith(riskFree_);
}

QuantLib::Date SsviBlackVol::maxDate() const {
    return QuantLib::Date::maxDate();
}

QuantLib::Real SsviBlackVol::minStrike() const {
    return QL_EPSILON;
}

QuantLib::Real SsviBlackVol::maxStrike() const {
    return QL_MAX_REAL;
}

QuantLib::Real SsviBlackVol::forward(QuantLib::Time t) const {
    return forwardOf(spot0_, dividend_, riskFree_, t);
}

QuantLib::Real SsviBlackVol::blackVarianceImpl(QuantLib::Time t, QuantLib::Real strike) const {
    const double k = std::log(strike / forward(t));
    return ssvi::w(surface_.node(t), k);
}

SsviLocalVol::SsviLocalVol(
    std::shared_ptr<SsviBlackVol> blackVol,
    QuantLib::Handle<QuantLib::Quote> spot,
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend,
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree)
    : QuantLib::LocalVolTermStructure(
          blackVol ? blackVol->referenceDate() : QuantLib::Date(),
          blackVol ? blackVol->calendar() : QuantLib::Calendar(),
          blackVol ? blackVol->businessDayConvention() : QuantLib::Following,
          blackVol ? blackVol->dayCounter() : QuantLib::DayCounter()),
      blackVol_(std::move(blackVol)), spot_(std::move(spot)), dividend_(std::move(dividend)),
      riskFree_(std::move(riskFree)) {
    if (!blackVol_) {
        QUANTRA_INVALID_ARGUMENT("SSVI local vol: black vol surface is required");
    }
    if (spot_.empty() || dividend_.empty() || riskFree_.empty()) {
        QUANTRA_INVALID_ARGUMENT("SSVI local vol: spot and curves of the process are required");
    }
    registerWith(spot_);
    registerWith(dividend_);
    registerWith(riskFree_);
    registerWith(blackVol_);
}

QuantLib::Date SsviLocalVol::maxDate() const {
    return QuantLib::Date::maxDate();
}

QuantLib::Real SsviLocalVol::minStrike() const {
    return QL_EPSILON;
}

QuantLib::Real SsviLocalVol::maxStrike() const {
    return QL_MAX_REAL;
}

QuantLib::Volatility SsviLocalVol::localVolImpl(QuantLib::Time t, QuantLib::Real strike) const {
    const double fRef = blackVol_->forward(t);
    const double fProc = forwardOf(spot_->value(), dividend_, riskFree_, t);
    const double k = std::log(strike / fRef);
    const double drift = std::log(fProc / fRef);
    const double y = k - drift;
    const double muProc = instantaneousForward(riskFree_, t) - instantaneousForward(dividend_, t);
    const double muRef = instantaneousForward(blackVol_->riskFree(), t) -
                         instantaneousForward(blackVol_->dividend(), t);

    const ssvi::Node node = blackVol_->surface().node(t);
    const double numerator = ssvi::wt(node, k) + ssvi::wk(node, k) * (muProc - muRef);
    const double gv = ssvi::g(node, k, y);
    if (!(gv > 0.0) || !(numerator >= 0.0)) {
        std::ostringstream os;
        os << "SSVI local variance undefined at t=" << t << ", S=" << strike << ": g=" << gv
           << ", numerator=" << numerator << " (surface admits arbitrage)";
        QUANTRA_ERROR(os.str());
    }
    return std::sqrt(numerator / gv);
}

} // namespace quantra
