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

#ifndef QUANTRA_WEIGHTED_ANDREASEN_HUGE_H
#define QUANTRA_WEIGHTED_ANDREASEN_HUGE_H

/**
 * Andreasen-Huge volatility interpolation (Andreasen J., Huge B., 2010,
 * "Volatility Interpolation", https://ssrn.com/abstract=1694972) with a
 * weight per calibration quote.
 *
 * This is QuantLib 1.44's AndreasenHugeVolatilityInterpl, in the one
 * configuration the engine uses (interpolation CubicSpline, calibration
 * CallPut), with these changes:
 *
 *   1. The two stages. QuantLib minimises, per expiry, the squared
 *      forward-normalised price errors of the put and of the call at each
 *      quoted strike, from a flat volatility. Here that calibration is the
 *      first stage, unchanged. The second stage calibrates a second
 *      surface, expiry by expiry, each expiry started from the first
 *      stage's volatilities of that expiry, by minimising
 *
 *          sum_i weight_i (surface vol_i - market vol_i)^2 / sum_i weight_i,
 *
 *      the surface vol at quote i read as blackVol() reads it at the
 *      expiry: the call prices on the grid, the spline, put-call parity
 *      below the forward, the implied standard deviation. Where the
 *      surface price has no implied volatility, the vol error is the price
 *      error over max(vega_i, kVegaFloor sqrt(T) pdf(0)), vega_i the
 *      forward-normalised Black vega of the quote at its market vol. At an
 *      expiry where the optimiser does not lower this cost, the second
 *      stage keeps its start.
 *
 *      The surface returned is the second stage's when the sum of its
 *      costs over the expiries is below the same sum for the first stage's
 *      surface, otherwise the first stage's. The result is therefore never
 *      worse than QuantLib's calibration in the weighted cost, and with all
 *      weights zero it is QuantLib's calibration.
 *
 *      Started from the flat volatility, a weighted cost alone does not
 *      settle on a close fit for every set of weights (weights that are
 *      large where the vega is small leave the volatility near the money
 *      free to collapse); started from the price fit it does. The cost is
 *      the vol the surface returns, not the price error over the vega: on
 *      quotes the model cannot match, the two do not move together.
 *   2. The calibration set is given as plain numbers per expiry (strike,
 *      market vol, weight) instead of instruments and quotes, and the
 *      calibration runs in the constructor (no lazy evaluation, no
 *      observer registration).
 *   3. calibrationError() takes, for each quote, the error of its
 *      out-of-the-money side (put below the forward, call from the forward
 *      up). QuantLib's CallPut branch compares the forward with a
 *      finite-difference grid point indexed by the quote, which selects the
 *      put side for every quote.
 *   4. blackVol() is the price inversion of QuantLib's
 *      AndreasenHugeVolatilityAdapter (a class bound to QuantLib's
 *      interpolation type), unchanged.
 *   5. The local vol accessors, the other interpolation and calibration
 *      types and the strike range arguments are left out; fits() reports
 *      how each expiry's optimisation ended.
 *
 * Everything else follows QuantLib: the finite-difference grid
 * (Concentrating1dMesher between min strike / 8 and 8 max strike,
 * concentrated at the spot), the implicit step per expiry, the cubic spline
 * of the piecewise volatility through the quoted strikes, the start (0.25)
 * of the first stage, the positive constraint, the optimiser
 * (Levenberg-Marquardt, QuantLib's default settings) and the end criteria
 * (500, 100, 1e-12, 1e-10, 1e-10), both used by each of the two stages.
 */

#include <ql/handle.hpp>
#include <ql/math/array.hpp>
#include <ql/math/interpolation.hpp>
#include <ql/math/optimization/endcriteria.hpp>
#include <ql/option.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/date.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace QuantLib {
class FdmMesherComposite;
}

namespace quantra {

class WeightedAndreasenHugeCostFunction;

class WeightedAndreasenHuge {
public:
    /// Floor of a quote's vega where the weighted cost falls back on the
    /// price error over the vega, as a fraction of the at-the-money vega of
    /// its expiry.
    static constexpr double kVegaFloor = 1e-2;

    struct Quote {
        double strike = 0.0;
        double vol = 0.0;
        double weight = 1.0;
    };
    /// The quotes of one expiry: distinct strikes, one quote each.
    struct Expiry {
        QuantLib::Date date;
        std::vector<Quote> quotes;
    };
    /// How the two stages ended at one expiry. The costs are the weighted
    /// mean squared vol error of the file comment, each on its own stage's
    /// surface; 0 when the weights of the expiry are all zero.
    struct ExpiryFit {
        QuantLib::Time time = 0.0;
        /// First stage: QuantLib's price fit from the flat volatility.
        QuantLib::EndCriteria::Type priceStageEndCriteria = QuantLib::EndCriteria::None;
        std::size_t priceStageEvaluations = 0;
        /// The weighted cost of the first stage's surface.
        double priceStageCost = 0.0;
        /// Second stage: the weighted fit from the first stage's
        /// volatilities. None and 0 when it did not run (its start has a
        /// zero cost).
        QuantLib::EndCriteria::Type weightedStageEndCriteria = QuantLib::EndCriteria::None;
        std::size_t weightedStageEvaluations = 0;
        /// The weighted cost of the second stage's surface.
        double weightedStageCost = 0.0;
        /// True when the second stage's optimiser lowered the cost of its
        /// start. False: the second stage's surface has the first stage's
        /// volatilities at this expiry.
        bool weightedStageKept = false;
        /// The weighted cost of the surface returned.
        double cost = 0.0;
        std::size_t functionEvaluations = 0;
    };

    /// Calibrates in two stages, each expiry by expiry (see the file
    /// comment). Throws QuantLib::Error when the set is empty, an expiry
    /// repeats or has no quote, a strike repeats within an expiry, a weight
    /// is negative or not finite, or the optimiser fails. Expiries and
    /// quotes may come in any order.
    WeightedAndreasenHuge(
        std::vector<Expiry> expiries,
        double spot,
        QuantLib::Handle<QuantLib::YieldTermStructure> rTS,
        QuantLib::Handle<QuantLib::YieldTermStructure> qTS,
        std::size_t nGridPoints = 500,
        double vegaFloor = kVegaFloor);
    ~WeightedAndreasenHuge();

    double minStrike() const;
    double maxStrike() const;
    QuantLib::Real fwd(QuantLib::Time t) const;

    /// min, max and average absolute error in volatility units (price
    /// error over the unfloored vega) over the calibration quotes.
    std::tuple<double, double, double> calibrationError() const;
    const std::vector<ExpiryFit>& fits() const { return fits_; }
    /// The sum over the expiries of the weighted cost of each stage's
    /// surface, and which of the two is returned (the second stage's only
    /// when its sum is the smaller).
    double priceStageCost() const { return priceStageCost_; }
    double weightedStageCost() const { return weightedStageCost_; }
    bool weightedStageUsed() const { return weightedStageUsed_; }

    QuantLib::Real optionPrice(
        QuantLib::Time t, QuantLib::Real strike, QuantLib::Option::Type optionType) const;

    /// The Black vol of the model price of the out-of-the-money option at
    /// (t, strike): QuantLib's adapter (implied standard deviation by
    /// Li's rational approximation polished with SOR, accuracy 1e-6, at
    /// most 1000 iterations), read as a BlackVarianceTermStructure reads it.
    QuantLib::Volatility blackVol(QuantLib::Time t, QuantLib::Real strike) const;
    /// The same at a date, the time measured on the risk-free curve.
    QuantLib::Volatility blackVol(const QuantLib::Date& d, QuantLib::Real strike) const;

    /// The finite-difference grid, ln(strike / forward) (QuantLib builds
    /// its range from ln(strike / spot) of the min and max strike).
    const QuantLib::Array& gridPoints() const { return gridPoints_; }

private:
    struct SingleStepCalibrationResult {
        QuantLib::Array putNPVs, callNPVs, sigmas;
        std::shared_ptr<WeightedAndreasenHugeCostFunction> costFunction;
    };
    struct PriceSlice {
        QuantLib::Real fwd = 0.0;
        std::shared_ptr<QuantLib::Array> prices;
        std::shared_ptr<QuantLib::Interpolation> interpolation;
    };

    std::shared_ptr<WeightedAndreasenHugeCostFunction> buildCostFunction(
        std::size_t iExpiry,
        QuantLib::Option::Type optionType,
        const QuantLib::Array& previousNPVs) const;
    void calibrate();
    std::size_t getExerciseTimeIdx(QuantLib::Time t) const;
    QuantLib::Array getPriceSlice(QuantLib::Time t) const;

    std::vector<Expiry> expiries_;
    const double spot_;
    const QuantLib::Handle<QuantLib::YieldTermStructure> rTS_;
    const QuantLib::Handle<QuantLib::YieldTermStructure> qTS_;
    const std::size_t nGridPoints_;
    const double vegaFloor_;

    double minQuotedStrike_ = 0.0, maxQuotedStrike_ = 0.0;
    std::size_t nQuotes_ = 0;
    std::vector<QuantLib::Time> expiryTimes_, dT_;
    double avgError_ = 0.0, minError_ = 0.0, maxError_ = 0.0;
    double priceStageCost_ = 0.0, weightedStageCost_ = 0.0;
    bool weightedStageUsed_ = false;

    std::shared_ptr<QuantLib::FdmMesherComposite> mesher_;
    QuantLib::Array gridPoints_;

    std::vector<SingleStepCalibrationResult> calibrationResults_;
    std::vector<ExpiryFit> fits_;

    mutable std::map<QuantLib::Time, PriceSlice> priceCache_;
};

} // namespace quantra

#endif // QUANTRA_WEIGHTED_ANDREASEN_HUGE_H
