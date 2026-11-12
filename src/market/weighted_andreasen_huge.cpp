/*
 Copyright (C) 2017, 2018 Klaus Spanderen

 This file is derived from QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/
 (ql/termstructures/volatility/equityfx/andreasenhugevolatilityinterpl.{hpp,cpp}
 and andreasenhugevolatilityadapter.{hpp,cpp} of QuantLib 1.44).

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <https://www.quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.

 The QuantLib license:

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

     Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.

     Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.

     Neither the names of the copyright holders nor the names of the QuantLib
     Group and its contributors may be used to endorse or promote products
     derived from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.
*/

#include "weighted_andreasen_huge.h"

#include <ql/errors.hpp>
#include <ql/instruments/payoffs.hpp>
#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/math/functional.hpp>
#include <ql/math/interpolations/backwardflatinterpolation.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/optimization/constraint.hpp>
#include <ql/math/optimization/costfunction.hpp>
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/math/optimization/problem.hpp>
#include <ql/methods/finitedifferences/meshers/concentrating1dmesher.hpp>
#include <ql/methods/finitedifferences/meshers/fdmmeshercomposite.hpp>
#include <ql/methods/finitedifferences/operators/fdmlinearoplayout.hpp>
#include <ql/methods/finitedifferences/operators/firstderivativeop.hpp>
#include <ql/methods/finitedifferences/operators/secondderivativeop.hpp>
#include <ql/methods/finitedifferences/operators/triplebandlinearop.hpp>
#include <ql/pricingengines/blackcalculator.hpp>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/utilities/null.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace quantra {

using QuantLib::Array;
using QuantLib::Real;
using QuantLib::Size;
using QuantLib::Time;

// One side (puts or calls) of one expiry step.
class WeightedAndreasenHugeCostFunction : public QuantLib::CostFunction {
public:
    WeightedAndreasenHugeCostFunction(
        Array marketNPVs,
        Array marketVegas,
        Array residualScales,
        Array lnMarketStrikes,
        Array previousNPVs,
        const std::shared_ptr<QuantLib::FdmMesherComposite>& mesher,
        Time dT)
        : marketNPVs_(std::move(marketNPVs)), marketVegas_(std::move(marketVegas)),
          residualScales_(std::move(residualScales)), lnMarketStrikes_(std::move(lnMarketStrikes)),
          previousNPVs_(std::move(previousNPVs)), mesher_(mesher), dT_(dT),
          cubicSpline_(lnMarketStrikes_.size() > 1),
          dxMap_(QuantLib::FirstDerivativeOp(0, mesher_)),
          dxxMap_(QuantLib::SecondDerivativeOp(0, mesher_)), mapT_(0, mesher_) {}

    // The prices one implicit step of length dT after `b`, under the
    // piecewise volatility `sig` given at the quoted strikes.
    Array solveFor(Time dT, const Array& sig, const Array& b) const {
        Array x(lnMarketStrikes_.size());
        QuantLib::Interpolation sigInterpl;

        if (cubicSpline_) {
            sigInterpl = QuantLib::CubicNaturalSpline(
                lnMarketStrikes_.begin(), lnMarketStrikes_.end(), sig.begin());
        } else {
            // A single quote: piecewise constant, as QuantLib falls back to.
            x.back() = lnMarketStrikes_.back();
            sigInterpl = QuantLib::BackwardFlatInterpolation(x.begin(), x.end(), sig.begin());
        }

        Array z(mesher_->layout()->size());
        for (const auto& iter : *mesher_->layout()) {
            const Size i = iter.index();
            const Real lnStrike = mesher_->location(iter, 0);

            const Real vol = sigInterpl(
                std::min(std::max(lnStrike, lnMarketStrikes_.front()), lnMarketStrikes_.back()),
                true);

            z[i] = 0.5 * vol * vol;
        }

        mapT_.axpyb(z, dxMap_, dxxMap_.mult(-z), Array());
        return mapT_.mult(Array(z.size(), dT)).solve_splitting(b, 1.0);
    }

    // Model price minus market price at every quoted strike
    // (forward-normalised): QuantLib's residuals.
    Array priceErrors(const Array& sig) const {
        Array newNPVs = solveFor(dT_, sig, previousNPVs_);

        const std::vector<Real>& gridPoints = mesher_->getFdm1dMeshers().front()->locations();

        const QuantLib::MonotonicCubicNaturalSpline interpl(
            gridPoints.begin(), gridPoints.end(), newNPVs.begin());

        Array retVal(lnMarketStrikes_.size());
        for (Size i = 0; i < retVal.size(); ++i) {
            const Real strike = lnMarketStrikes_[i];
            retVal[i] = interpl(strike) - marketNPVs_[i];
        }
        return retVal;
    }

    // The weighted residuals: sqrt(weight) * price error / floored vega.
    Array values(const Array& sig) const override { return priceErrors(sig) * residualScales_; }

    Array vegaCalibrationError(const Array& sig) const { return priceErrors(sig) / marketVegas_; }

    Array initialValues() const { return Array(lnMarketStrikes_.size(), 0.25); }

    const Array& lnMarketStrikes() const { return lnMarketStrikes_; }

private:
    const Array marketNPVs_, marketVegas_, residualScales_;
    const Array lnMarketStrikes_, previousNPVs_;
    const std::shared_ptr<QuantLib::FdmMesherComposite> mesher_;
    const Time dT_;
    const bool cubicSpline_;

    const QuantLib::FirstDerivativeOp dxMap_;
    const QuantLib::TripleBandLinearOp dxxMap_;
    mutable QuantLib::TripleBandLinearOp mapT_;
};

namespace {

// Put residuals followed by call residuals.
class CombinedCostFunction : public QuantLib::CostFunction {
public:
    CombinedCostFunction(
        std::shared_ptr<WeightedAndreasenHugeCostFunction> putCostFct,
        std::shared_ptr<WeightedAndreasenHugeCostFunction> callCostFct)
        : putCostFct_(std::move(putCostFct)), callCostFct_(std::move(callCostFct)) {}

    Array values(const Array& sig) const override {
        const Array pv = putCostFct_->values(sig);
        const Array cv = callCostFct_->values(sig);

        Array retVal(pv.size() + cv.size());
        std::copy(pv.begin(), pv.end(), retVal.begin());
        std::copy(cv.begin(), cv.end(), retVal.begin() + cv.size());

        return retVal;
    }

    Array initialValues() const {
        return 0.5 * (putCostFct_->initialValues() + callCostFct_->initialValues());
    }

private:
    const std::shared_ptr<WeightedAndreasenHugeCostFunction> putCostFct_;
    const std::shared_ptr<WeightedAndreasenHugeCostFunction> callCostFct_;
};

} // namespace

WeightedAndreasenHuge::WeightedAndreasenHuge(
    std::vector<Expiry> expiries,
    double spot,
    QuantLib::Handle<QuantLib::YieldTermStructure> rTS,
    QuantLib::Handle<QuantLib::YieldTermStructure> qTS,
    std::size_t nGridPoints,
    double vegaFloor)
    : expiries_(std::move(expiries)), spot_(spot), rTS_(std::move(rTS)), qTS_(std::move(qTS)),
      nGridPoints_(nGridPoints), vegaFloor_(vegaFloor) {
    QL_REQUIRE(nGridPoints_ > 2 && !expiries_.empty(), "undefined grid or calibration set");

    std::sort(expiries_.begin(), expiries_.end(), [](const Expiry& a, const Expiry& b) {
        return a.date < b.date;
    });
    minQuotedStrike_ = std::numeric_limits<double>::max();
    maxQuotedStrike_ = 0.0;
    for (Size i = 0; i < expiries_.size(); ++i) {
        auto& quotes = expiries_[i].quotes;
        QL_REQUIRE(
            i == 0 || expiries_[i - 1].date < expiries_[i].date,
            "expiry " << expiries_[i].date << " given twice");
        QL_REQUIRE(!quotes.empty(), "no quote at expiry " << expiries_[i].date);
        std::sort(quotes.begin(), quotes.end(), [](const Quote& a, const Quote& b) {
            return a.strike < b.strike;
        });
        for (Size j = 0; j < quotes.size(); ++j) {
            QL_REQUIRE(
                j == 0 || quotes[j - 1].strike < quotes[j].strike,
                "strike " << quotes[j].strike << " given twice at expiry " << expiries_[i].date);
            QL_REQUIRE(
                quotes[j].weight >= 0.0 && std::isfinite(quotes[j].weight),
                "weight of strike " << quotes[j].strike << " at expiry " << expiries_[i].date
                                    << " must be finite and >= 0");
        }
        minQuotedStrike_ = std::min(minQuotedStrike_, quotes.front().strike);
        maxQuotedStrike_ = std::max(maxQuotedStrike_, quotes.back().strike);
        nQuotes_ += quotes.size();
    }

    dT_.resize(expiries_.size());
    expiryTimes_.resize(expiries_.size());

    calibrate();
}

WeightedAndreasenHuge::~WeightedAndreasenHuge() = default;

std::shared_ptr<WeightedAndreasenHugeCostFunction> WeightedAndreasenHuge::buildCostFunction(
    Size iExpiry, QuantLib::Option::Type optionType, const Array& previousNPVs) const {

    const Time expiryTime = expiryTimes_[iExpiry];

    const QuantLib::DiscountFactor discount = rTS_->discount(expiryTime);
    const Real fwd = spot_ * qTS_->discount(expiryTime) / discount;

    const auto& quotes = expiries_[iExpiry].quotes;
    const Size nOptions = quotes.size();

    Array lnMarketStrikes(nOptions), marketNPVs(nOptions), marketVegas(nOptions);
    Array residualScales(nOptions);

    // The at-the-money vega of the expiry, forward-normalised.
    const Real floor = vegaFloor_ * std::sqrt(expiryTime) *
                       QuantLib::CumulativeNormalDistribution().derivative(0.0);

    // calculate undiscounted market prices
    for (Size k = 0; k < nOptions; ++k) {
        const QuantLib::Volatility vol = quotes[k].vol;
        const Real stdDev = vol * std::sqrt(expiryTime);

        const QuantLib::BlackCalculator calculator(
            optionType, quotes[k].strike, fwd, stdDev, discount);

        const Real npv = calculator.value();
        const Real vega = calculator.vega(expiryTime);

        marketNPVs[k] = npv / (discount * fwd);
        marketVegas[k] = vega / (discount * fwd);
        lnMarketStrikes[k] = std::log(quotes[k].strike / fwd);
        residualScales[k] = std::sqrt(quotes[k].weight) / std::max(marketVegas[k], floor);
    }

    return std::make_shared<WeightedAndreasenHugeCostFunction>(
        marketNPVs, marketVegas, residualScales, lnMarketStrikes, previousNPVs, mesher_,
        dT_[iExpiry]);
}

void WeightedAndreasenHuge::calibrate() {
    QL_REQUIRE(maxStrike() > minStrike(), "max strike must be greater than min strike");

    const QuantLib::DayCounter dc = rTS_->dayCounter();
    for (Size i = 0; i < expiryTimes_.size(); ++i) {
        expiryTimes_[i] = dc.yearFraction(rTS_->referenceDate(), expiries_[i].date);
        dT_[i] = expiryTimes_[i] - ((i == 0) ? 0.0 : expiryTimes_[i - 1]);
    }

    mesher_ = std::make_shared<QuantLib::FdmMesherComposite>(
        std::make_shared<QuantLib::Concentrating1dMesher>(
            std::log(minStrike() / spot_), std::log(maxStrike() / spot_), nGridPoints_,
            std::pair<Real, Real>(0.0, 0.025)));

    gridPoints_ = mesher_->locations(0);

    avgError_ = 0.0;
    minError_ = std::numeric_limits<Real>::max();
    maxError_ = 0.0;

    calibrationResults_.reserve(expiries_.size());
    fits_.reserve(expiries_.size());

    Array npvPuts(nGridPoints_);
    Array npvCalls(nGridPoints_);

    for (Size i = 0; i < nGridPoints_; ++i) {
        const Real strike = std::exp(gridPoints_[i]);
        npvPuts[i] = QuantLib::PlainVanillaPayoff(QuantLib::Option::Put, strike)(1.0);
        npvCalls[i] = QuantLib::PlainVanillaPayoff(QuantLib::Option::Call, strike)(1.0);
    }

    const QuantLib::EndCriteria endCriteria(500, 100, 1e-12, 1e-10, 1e-10);

    for (Size i = 0; i < expiries_.size(); ++i) {
        const std::shared_ptr<WeightedAndreasenHugeCostFunction> putCostFct =
            buildCostFunction(i, QuantLib::Option::Put, npvPuts);
        const std::shared_ptr<WeightedAndreasenHugeCostFunction> callCostFct =
            buildCostFunction(i, QuantLib::Option::Call, npvCalls);

        CombinedCostFunction costFunction(putCostFct, callCostFct);

        QuantLib::PositiveConstraint positiveConstraint;
        QuantLib::Problem problem(costFunction, positiveConstraint, costFunction.initialValues());

        QuantLib::LevenbergMarquardt optimizationMethod;
        const QuantLib::EndCriteria::Type ended = optimizationMethod.minimize(problem, endCriteria);

        const Array& sig = problem.currentValue();

        calibrationResults_.push_back(
            SingleStepCalibrationResult{npvPuts, npvCalls, sig, putCostFct});

        ExpiryFit fit;
        fit.time = expiryTimes_[i];
        fit.endCriteria = ended;
        fit.functionEvaluations = problem.functionEvaluation();
        {
            const Array r = costFunction.values(sig);
            fit.cost = QuantLib::DotProduct(r, r);
        }
        fits_.push_back(fit);

        // The error of each quote's out-of-the-money side.
        const Array vegaPutDiffs = putCostFct->vegaCalibrationError(sig);
        const Array vegaCallDiffs = callCostFct->vegaCalibrationError(sig);
        const Array& lnMarketStrikes = putCostFct->lnMarketStrikes();

        Array vegaDiffs(sig.size());
        for (Size j = 0; j < vegaDiffs.size(); ++j)
            vegaDiffs[j] =
                std::fabs((lnMarketStrikes[j] < 0.0) ? vegaPutDiffs[j] : vegaCallDiffs[j]);

        avgError_ += std::accumulate(vegaDiffs.begin(), vegaDiffs.end(), Real(0.0));
        minError_ = std::min(minError_, *std::min_element(vegaDiffs.begin(), vegaDiffs.end()));
        maxError_ = std::max(maxError_, *std::max_element(vegaDiffs.begin(), vegaDiffs.end()));

        npvPuts = putCostFct->solveFor(dT_[i], sig, npvPuts);
        npvCalls = callCostFct->solveFor(dT_[i], sig, npvCalls);
    }

    avgError_ /= static_cast<Real>(nQuotes_);
}

double WeightedAndreasenHuge::minStrike() const {
    return 1 / 8.0 * minQuotedStrike_;
}

double WeightedAndreasenHuge::maxStrike() const {
    return 8.0 * maxQuotedStrike_;
}

Real WeightedAndreasenHuge::fwd(Time t) const {
    return spot_ * qTS_->discount(t) / rTS_->discount(t);
}

std::tuple<double, double, double> WeightedAndreasenHuge::calibrationError() const {
    return std::make_tuple(minError_, maxError_, avgError_);
}

Size WeightedAndreasenHuge::getExerciseTimeIdx(Time t) const {
    return std::min<Size>(
        expiryTimes_.size() - 1,
        std::distance(
            expiryTimes_.begin(), std::upper_bound(expiryTimes_.begin(), expiryTimes_.end(), t)));
}

// The call prices on the grid at time t.
Array WeightedAndreasenHuge::getPriceSlice(Time t) const {
    const Size iu = getExerciseTimeIdx(t);

    return calibrationResults_[iu].costFunction->solveFor(
        (iu == 0) ? t : t - expiryTimes_[iu - 1], calibrationResults_[iu].sigmas,
        calibrationResults_[iu].callNPVs);
}

Real WeightedAndreasenHuge::optionPrice(
    Time t, Real strike, QuantLib::Option::Type optionType) const {

    auto f = priceCache_.find(t);

    const QuantLib::DiscountFactor df = rTS_->discount(t);

    if (f == priceCache_.end()) {
        PriceSlice slice;
        slice.prices = std::make_shared<Array>(getPriceSlice(t));
        slice.fwd = spot_ * qTS_->discount(t) / df;
        slice.interpolation = std::make_shared<QuantLib::CubicNaturalSpline>(
            gridPoints_.begin() + 1, gridPoints_.end() - 1, slice.prices->begin() + 1);
        f = priceCache_.emplace(t, std::move(slice)).first;
    }

    const Real fwd = f->second.fwd;
    const Real k = std::log(strike / fwd);
    const Real s = std::max(gridPoints_[1], std::min(*(gridPoints_.end() - 2), k));

    Real price = (*f->second.interpolation)(s);

    if (optionType == QuantLib::Option::Put) price = price + strike / fwd - 1.0;

    return price * df * fwd;
}

QuantLib::Volatility WeightedAndreasenHuge::blackVol(Time t, Real strike) const {
    const Time nonZeroMaturity = (t == 0.0 ? 0.00001 : t);

    const Real fwd = this->fwd(nonZeroMaturity);
    const QuantLib::Option::Type optionType =
        (fwd > strike) ? QuantLib::Option::Put : QuantLib::Option::Call;

    const Real npv = optionPrice(nonZeroMaturity, strike, optionType);

    const Real var = QuantLib::squared(
        QuantLib::blackFormulaImpliedStdDevLiRS(
            optionType, strike, fwd, npv, rTS_->discount(nonZeroMaturity), 0.0,
            QuantLib::Null<Real>(), 1.0, 1e-6, 1000));

    return std::sqrt(var / nonZeroMaturity);
}

QuantLib::Volatility WeightedAndreasenHuge::blackVol(const QuantLib::Date& d, Real strike) const {
    return blackVol(rTS_->dayCounter().yearFraction(rTS_->referenceDate(), d), strike);
}

} // namespace quantra
