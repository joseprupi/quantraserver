#include "smile_slice_vol_surface.h"

#include <ql/errors.hpp>
#include <ql/utilities/null.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace quantra {

using QuantLib::Real;
using QuantLib::Time;

// ---------------------------------------------------------------------------
// Slices
// ---------------------------------------------------------------------------

CubicVarianceSlice::CubicVarianceSlice(
    const std::vector<double>& strikes, std::vector<double> totalVariances)
    : variances_(std::move(totalVariances)) {
    QL_REQUIRE(strikes.size() >= 2, "cubic smile slice needs at least 2 strikes");
    QL_REQUIRE(
        strikes.size() == variances_.size(), "cubic smile slice: strikes and variances differ");
    logStrikes_.reserve(strikes.size());
    for (std::size_t j = 0; j < strikes.size(); ++j) {
        QL_REQUIRE(strikes[j] > 0.0, "cubic smile slice: strike must be positive");
        QL_REQUIRE(
            j == 0 || strikes[j] > strikes[j - 1],
            "cubic smile slice: strikes must be strictly increasing");
        logStrikes_.push_back(std::log(strikes[j]));
    }
    spline_ = std::make_unique<QuantLib::CubicNaturalSpline>(
        logStrikes_.begin(), logStrikes_.end(), variances_.begin());
}

double CubicVarianceSlice::totalVariance(double strike) const {
    // Constant beyond the edges: the edge node's total variance, as the
    // bilinear surface's ConstantExtrapolation.
    const double x = std::min(std::max(std::log(strike), logStrikes_.front()), logStrikes_.back());
    return (*spline_)(x, true);
}

double sviRawTotalVariance(double a, double b, double sigma, double rho, double m, double k) {
    const double d = k - m;
    return a + b * (rho * d + std::sqrt(d * d + sigma * sigma));
}

double SviSmileSlice::totalVariance(double strike) const {
    // QuantLib's SviSmileSection floors the strike at 1e-6.
    const double k = std::log(std::max(strike, 1.0e-6) / p_.forward);
    return sviRawTotalVariance(p_.a, p_.b, p_.sigma, p_.rho, p_.m, k);
}

// ---------------------------------------------------------------------------
// Surface forwards
// ---------------------------------------------------------------------------

NodeSurfaceForward::NodeSurfaceForward(
    const std::vector<Time>& times,
    const std::vector<double>& forwards,
    std::optional<double> spot) {
    QL_REQUIRE(!times.empty(), "surface forward needs at least one node");
    QL_REQUIRE(times.size() == forwards.size(), "surface forward: times and forwards differ");
    if (spot) {
        QL_REQUIRE(
            std::isfinite(*spot) && *spot > 0.0, "surface forward: spot must be finite and > 0");
        times_.push_back(0.0);
        forwards_.push_back(*spot);
        logForwards_.push_back(std::log(*spot));
    }
    for (std::size_t i = 0; i < times.size(); ++i) {
        QL_REQUIRE(
            std::isfinite(forwards[i]) && forwards[i] > 0.0,
            "surface forward: forward " << i << " must be finite and > 0");
        QL_REQUIRE(times[i] > 0.0, "surface forward: node times must be positive");
        QL_REQUIRE(
            times_.empty() || times[i] > times_.back(),
            "surface forward: node times must be strictly increasing");
        times_.push_back(times[i]);
        forwards_.push_back(forwards[i]);
        logForwards_.push_back(std::log(forwards[i]));
    }
}

double NodeSurfaceForward::forward(Time t) const {
    const std::size_t n = times_.size();
    if (n == 1) return forwards_.front();
    // The segment holding t; the first / last one continued beyond the ends.
    std::size_t j = static_cast<std::size_t>(
        std::upper_bound(times_.begin(), times_.end(), t) - times_.begin());
    if (j > 0 && times_[j - 1] == t) return forwards_[j - 1];
    j = j == 0 ? 0 : std::min(j - 1, n - 2);
    const double l = logForwards_[j] + (logForwards_[j + 1] - logForwards_[j]) * (t - times_[j]) /
                                           (times_[j + 1] - times_[j]);
    return std::exp(l);
}

CurveSurfaceForward::CurveSurfaceForward(
    double spot,
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend,
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree)
    : spot_(spot), dividend_(std::move(dividend)), riskFree_(std::move(riskFree)) {
    QL_REQUIRE(std::isfinite(spot_) && spot_ > 0.0, "surface forward: spot must be finite and > 0");
    QL_REQUIRE(
        !dividend_.empty() && !riskFree_.empty(),
        "surface forward: dividend and risk-free curves are required");
}

double CurveSurfaceForward::forward(Time t) const {
    return spot_ * dividend_->discount(t, true) / riskFree_->discount(t, true);
}

ProcessShapedSurfaceForward::ProcessShapedSurfaceForward(
    std::shared_ptr<const SurfaceForward> base,
    std::vector<Time> expiryTimes,
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend,
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree)
    : base_(std::move(base)), dividend_(std::move(dividend)), riskFree_(std::move(riskFree)) {
    QL_REQUIRE(base_, "process-shaped forward: no surface forward");
    QL_REQUIRE(
        !dividend_.empty() && !riskFree_.empty(),
        "process-shaped forward: dividend and risk-free curves are required");
    times_.push_back(0.0);
    for (Time t : expiryTimes) {
        QL_REQUIRE(t > times_.back(), "process-shaped forward: expiry times must increase");
        times_.push_back(t);
    }
}

double ProcessShapedSurfaceForward::g(Time t) const {
    return std::log(dividend_->discount(t, true) / riskFree_->discount(t, true));
}

double ProcessShapedSurfaceForward::forward(Time t) const {
    const double F = base_->forward(t);
    const std::size_t n = times_.size();
    if (n < 2) return F;
    std::size_t j = static_cast<std::size_t>(
        std::upper_bound(times_.begin(), times_.end(), t) - times_.begin());
    if (j > 0 && times_[j - 1] == t) return F; // at a node g - ĝ is 0
    j = j == 0 ? 0 : std::min(j - 1, n - 2);
    const double g0 = j == 0 ? 0.0 : g(times_[j]);
    const double g1 = g(times_[j + 1]);
    const double gHat = g0 + (g1 - g0) * (t - times_[j]) / (times_[j + 1] - times_[j]);
    return F * std::exp(g(t) - gHat);
}

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------

SmileSliceVolSurface::SmileSliceVolSurface(
    const QuantLib::Date& referenceDate,
    const QuantLib::Calendar& calendar,
    QuantLib::BusinessDayConvention bdc,
    const QuantLib::DayCounter& dayCounter,
    std::vector<QuantLib::Date> expiries,
    std::vector<std::shared_ptr<const SmileSlice>> slices,
    Real minStrike,
    Real maxStrike,
    std::shared_ptr<const SurfaceForward> forward)
    : QuantLib::BlackVarianceTermStructure(referenceDate, calendar, bdc, dayCounter),
      expiries_(std::move(expiries)), slices_(std::move(slices)), minStrike_(minStrike),
      maxStrike_(maxStrike), forward_(std::move(forward)) {
    QL_REQUIRE(!expiries_.empty(), "smile slice surface needs at least one expiry");
    QL_REQUIRE(
        expiries_.size() == slices_.size(), "smile slice surface: expiries and slices differ");
    times_.reserve(expiries_.size());
    for (std::size_t i = 0; i < expiries_.size(); ++i) {
        QL_REQUIRE(slices_[i], "smile slice surface: null slice");
        QL_REQUIRE(
            expiries_[i] > referenceDate, "smile slice surface: expiry "
                                              << expiries_[i] << " is not after the reference date "
                                              << referenceDate);
        QL_REQUIRE(
            i == 0 || expiries_[i] > expiries_[i - 1],
            "smile slice surface: expiries must be strictly increasing");
        times_.push_back(timeFromReference(expiries_[i]));
    }
    if (forward_) {
        for (Time T : times_) {
            const double F = forward_->forward(T);
            QL_REQUIRE(
                std::isfinite(F) && F > 0.0,
                "smile slice surface: the surface forward at " << T << " is not positive");
            sliceForwards_.push_back(F);
        }
    }
}

std::shared_ptr<SmileSliceVolSurface> SmileSliceVolSurface::withSurfaceForward(
    std::shared_ptr<const SurfaceForward> forward) const {
    auto s = std::make_shared<SmileSliceVolSurface>(
        referenceDate(), calendar(), businessDayConvention(), dayCounter(), expiries_, slices_,
        minStrike_, maxStrike_, std::move(forward));
    if (allowsExtrapolation()) {
        s->enableExtrapolation();
    } else {
        s->disableExtrapolation();
    }
    return s;
}

Real SmileSliceVolSurface::blackVarianceImpl(Time t, Real strike) const {
    if (forward_) {
        // Fixed forward moneyness: slice i read at K (F_i / F(t)), the
        // ratio first so that at t = T_i the strike is K exactly.
        const double F = forward_->forward(t);
        const auto w = [&](std::size_t i) {
            return slices_[i]->totalVariance(strike * (sliceForwards_[i] / F));
        };
        if (t <= times_.front()) return w(0) * t / times_.front();
        if (t >= times_.back()) return w(times_.size() - 1) * t / times_.back();
        const std::size_t i =
            static_cast<std::size_t>(
                std::upper_bound(times_.begin(), times_.end(), t) - times_.begin()) -
            1;
        const double w0 = w(i);
        const double w1 = w(i + 1);
        return w0 + (w1 - w0) * (t - times_[i]) / (times_[i + 1] - times_[i]);
    }
    if (t <= times_.front()) {
        return slices_.front()->totalVariance(strike) * t / times_.front();
    }
    if (t >= times_.back()) {
        return slices_.back()->totalVariance(strike) * t / times_.back();
    }
    // times_[i] <= t < times_[i + 1]
    const std::size_t i = static_cast<std::size_t>(
                              std::upper_bound(times_.begin(), times_.end(), t) - times_.begin()) -
                          1;
    const double w0 = slices_[i]->totalVariance(strike);
    const double w1 = slices_[i + 1]->totalVariance(strike);
    return w0 + (w1 - w0) * (t - times_[i]) / (times_[i + 1] - times_[i]);
}

// ---------------------------------------------------------------------------
// Shift
// ---------------------------------------------------------------------------

ShiftedBlackVolSurface::ShiftedBlackVolSurface(
    std::shared_ptr<const QuantLib::BlackVolTermStructure> inner, double shift)
    : QuantLib::BlackVarianceTermStructure(inner->businessDayConvention(), inner->dayCounter()),
      inner_(std::move(inner)), shift_(shift) {}

Real ShiftedBlackVolSurface::blackVarianceImpl(Time t, Real strike) const {
    const double v = inner_->blackVol(t, strike, true) + shift_;
    QL_REQUIRE(
        v > 0.0, "Black vol shift " << shift_ << " makes the volatility non-positive (" << v
                                    << ") at time " << t << " strike " << strike);
    return v * v * t;
}

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

std::shared_ptr<SmileSliceVolSurface> makeCubicStrikeLinearTimeSurface(
    const QuantLib::Date& referenceDate,
    const QuantLib::Calendar& calendar,
    QuantLib::BusinessDayConvention bdc,
    const QuantLib::DayCounter& dayCounter,
    const std::vector<QuantLib::Date>& expiries,
    const std::vector<double>& strikes,
    const std::vector<std::vector<double>>& blackVolsByExpiry,
    std::shared_ptr<const SurfaceForward> forward) {
    QL_REQUIRE(strikes.size() >= 2, "CubicStrikeLinearTime needs at least 2 strikes");
    QL_REQUIRE(
        blackVolsByExpiry.size() == expiries.size(),
        "CubicStrikeLinearTime: one vol row per expiry is required");
    std::vector<std::shared_ptr<const SmileSlice>> slices;
    slices.reserve(expiries.size());
    for (std::size_t i = 0; i < expiries.size(); ++i) {
        const Time T = dayCounter.yearFraction(referenceDate, expiries[i]);
        const auto& row = blackVolsByExpiry[i];
        QL_REQUIRE(row.size() == strikes.size(), "CubicStrikeLinearTime: vol row length");
        std::vector<double> w(row.size());
        for (std::size_t j = 0; j < row.size(); ++j) {
            QL_REQUIRE(row[j] > 0.0, "CubicStrikeLinearTime: Black vol must be positive");
            w[j] = row[j] * row[j] * T;
        }
        slices.push_back(std::make_shared<CubicVarianceSlice>(strikes, std::move(w)));
    }
    return std::make_shared<SmileSliceVolSurface>(
        referenceDate, calendar, bdc, dayCounter, expiries, std::move(slices), strikes.front(),
        strikes.back(), std::move(forward));
}

std::shared_ptr<SmileSliceVolSurface> makeSviSurface(
    const QuantLib::Date& referenceDate,
    const QuantLib::Calendar& calendar,
    QuantLib::BusinessDayConvention bdc,
    const QuantLib::DayCounter& dayCounter,
    const std::vector<QuantLib::Date>& expiries,
    const std::vector<SviSliceParams>& params,
    SviTimeReading reading,
    std::optional<double> spot) {
    QL_REQUIRE(params.size() == expiries.size(), "SVI surface: one slice per expiry is required");
    std::vector<std::shared_ptr<const SmileSlice>> slices;
    slices.reserve(params.size());
    for (const auto& p : params) {
        QL_REQUIRE(p.forward > 0.0, "SVI slice: forward must be positive");
        QL_REQUIRE(p.b >= 0.0, "SVI slice: b must be >= 0");
        QL_REQUIRE(p.sigma > 0.0, "SVI slice: sigma must be > 0");
        QL_REQUIRE(std::fabs(p.rho) < 1.0, "SVI slice: rho must be in (-1, 1)");
        QL_REQUIRE(
            p.a + p.b * p.sigma * std::sqrt(1.0 - p.rho * p.rho) >= 0.0,
            "SVI slice: a + b sigma sqrt(1 - rho^2) must be >= 0 (negative minimum variance)");
        slices.push_back(std::make_shared<SviSmileSlice>(p));
    }
    std::shared_ptr<const SurfaceForward> forward;
    if (reading == SviTimeReading::FixedForwardMoneyness) {
        std::vector<Time> times;
        std::vector<double> forwards;
        for (std::size_t i = 0; i < params.size(); ++i) {
            times.push_back(dayCounter.yearFraction(referenceDate, expiries[i]));
            forwards.push_back(params[i].forward);
        }
        forward = std::make_shared<NodeSurfaceForward>(times, forwards, spot);
    }
    return std::make_shared<SmileSliceVolSurface>(
        referenceDate, calendar, bdc, dayCounter, expiries, std::move(slices), 0.0, QL_MAX_REAL,
        std::move(forward));
}

// ---------------------------------------------------------------------------
// The surface a note's local vol reads
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<SmileSliceVolSurface> processShaped(
    const SmileSliceVolSurface& s,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& dividend,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFree) {
    return s.withSurfaceForward(
        std::make_shared<ProcessShapedSurfaceForward>(
            s.surfaceForward(), s.times(), dividend, riskFree));
}

} // namespace

std::shared_ptr<QuantLib::BlackVolTermStructure> localVolReadingSurface(
    const std::shared_ptr<QuantLib::BlackVolTermStructure>& vol,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& dividend,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFree) {
    if (const auto* s = dynamic_cast<const SmileSliceVolSurface*>(vol.get())) {
        if (s->surfaceForward()) return processShaped(*s, dividend, riskFree);
        return vol;
    }
    if (const auto* sh = dynamic_cast<const ShiftedBlackVolSurface*>(vol.get())) {
        const auto* s = dynamic_cast<const SmileSliceVolSurface*>(sh->inner().get());
        if (s && s->surfaceForward()) {
            auto out = std::make_shared<ShiftedBlackVolSurface>(
                processShaped(*s, dividend, riskFree), sh->shift());
            if (vol->allowsExtrapolation()) out->enableExtrapolation();
            return out;
        }
    }
    return vol;
}

} // namespace quantra
