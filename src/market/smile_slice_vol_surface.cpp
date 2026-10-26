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
    Real maxStrike)
    : QuantLib::BlackVarianceTermStructure(referenceDate, calendar, bdc, dayCounter),
      expiries_(std::move(expiries)), slices_(std::move(slices)), minStrike_(minStrike),
      maxStrike_(maxStrike) {
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
}

Real SmileSliceVolSurface::blackVarianceImpl(Time t, Real strike) const {
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
    const std::vector<std::vector<double>>& blackVolsByExpiry) {
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
        strikes.back());
}

std::shared_ptr<SmileSliceVolSurface> makeSviSurface(
    const QuantLib::Date& referenceDate,
    const QuantLib::Calendar& calendar,
    QuantLib::BusinessDayConvention bdc,
    const QuantLib::DayCounter& dayCounter,
    const std::vector<QuantLib::Date>& expiries,
    const std::vector<SviSliceParams>& params) {
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
    return std::make_shared<SmileSliceVolSurface>(
        referenceDate, calendar, bdc, dayCounter, expiries, std::move(slices), 0.0, QL_MAX_REAL);
}

} // namespace quantra
