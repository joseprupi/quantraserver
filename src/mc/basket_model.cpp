#include "basket_model.h"

#include "error.h"

#include <ql/interestrate.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancecurve.hpp>
#include <ql/termstructures/volatility/equityfx/localvolsurface.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>

namespace quantra { namespace mc {

bool IsExactStepCapable(const QuantLib::Handle<QuantLib::BlackVolTermStructure>& vol) {
    if (vol.empty()) {
        QUANTRA_ERROR("basket model: Black vol handle must be linked");
    }
    // Mirrors GeneralizedBlackScholesProcess::localVolatility(): a
    // BlackConstantVol or a BlackVarianceCurve is strike-independent and gets
    // the exact lognormal step; anything else goes through LocalVolSurface.
    const auto& ts = vol.currentLink();
    if (std::dynamic_pointer_cast<QuantLib::BlackConstantVol>(ts)) {
        return true;
    }
    if (std::dynamic_pointer_cast<QuantLib::BlackVarianceCurve>(ts)) {
        return true;
    }
    return false;
}

BasketAsset MakeBasketAsset(
    const std::string& id,
    const QuantLib::Handle<QuantLib::Quote>& spot,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& dividend,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFree,
    const QuantLib::Handle<QuantLib::BlackVolTermStructure>& vol) {
    BasketAsset asset;
    asset.id = id;
    asset.spot = spot;
    asset.dividend = dividend;
    asset.riskFree = riskFree;
    asset.vol = vol;
    asset.exactStepCapable = IsExactStepCapable(vol);
    return asset;
}

EdgeClampedLocalVol::EdgeClampedLocalVol(
    std::shared_ptr<QuantLib::LocalVolTermStructure> inner,
    QuantLib::Real minStrike,
    QuantLib::Real maxStrike,
    QuantLib::Time maxTime)
    : QuantLib::LocalVolTermStructure(inner->businessDayConvention(), inner->dayCounter()),
      inner_(std::move(inner)), lower_(minStrike * std::exp(kEdgeMargin)),
      upper_(maxStrike * std::exp(-kEdgeMargin)), tMax_(QL_MAX_REAL) {
    if (maxTime != QuantLib::Null<QuantLib::Time>() && maxTime > 2.0 * kTimeMargin) {
        tMax_ = maxTime - kTimeMargin;
    }
    if (!(lower_ < upper_)) {
        // A strike range narrower than twice the margin (or unbounded on
        // one side only): no interior to clamp to, evaluate as is.
        lower_ = minStrike;
        upper_ = maxStrike;
    }
    registerWith(inner_);
}

EdgeClampedLocalVol::EdgeClampedLocalVol(
    std::shared_ptr<QuantLib::LocalVolTermStructure> inner,
    const std::vector<LocalVolStrikeBand>& bands,
    QuantLib::Time maxTime)
    : EdgeClampedLocalVol(
          std::move(inner),
          bands.empty() ? 0.0 : bands.front().minStrike,
          bands.empty() ? QL_MAX_REAL : bands.front().maxStrike,
          maxTime) {
    if (bands.empty()) {
        QUANTRA_ERROR("basket model: per-expiry local vol strike bands must not be empty");
    }
    for (std::size_t i = 0; i < bands.size(); ++i) {
        LocalVolStrikeBand b = bands[i];
        if (!(b.minStrike < b.maxStrike) || (i > 0 && !(b.time > bands[i - 1].time))) {
            QUANTRA_ERROR("basket model: invalid per-expiry local vol strike band");
        }
        const QuantLib::Real lo = b.minStrike * std::exp(kEdgeMargin);
        const QuantLib::Real hi = b.maxStrike * std::exp(-kEdgeMargin);
        if (lo < hi) {
            b.minStrike = lo;
            b.maxStrike = hi;
        }
        bands_.push_back(b);
    }
}

QuantLib::Real EdgeClampedLocalVol::clampedLevel(QuantLib::Real s, QuantLib::Time t) const {
    if (bands_.empty()) return std::min(std::max(s, lower_), upper_);
    auto it = std::lower_bound(
        bands_.begin(), bands_.end(), t,
        [](const LocalVolStrikeBand& b, QuantLib::Time x) { return b.time < x; });
    if (it == bands_.end()) --it;
    return std::min(std::max(s, it->minStrike), it->maxStrike);
}

QuantLib::Time EdgeClampedLocalVol::clampedTime(QuantLib::Time t) const {
    return std::min(t, tMax_);
}

void EdgeClampedLocalVol::setFloor(
    QuantLib::Real floor,
    QuantLib::Handle<QuantLib::BlackVolTermStructure> black,
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree,
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend,
    QuantLib::Handle<QuantLib::Quote> spot) {
    if (!(floor > 0.0) || black.empty() || riskFree.empty() || dividend.empty() || spot.empty()) {
        QUANTRA_ERROR("basket model: a local vol floor needs a floor > 0 and linked handles");
    }
    floor_ = floor;
    black_ = std::move(black);
    riskFree_ = std::move(riskFree);
    dividend_ = std::move(dividend);
    spot_ = std::move(spot);
}

QuantLib::Real EdgeClampedLocalVol::dupire(QuantLib::Time t, QuantLib::Real underlyingLevel) const {
    // QuantLib 1.44 LocalVolSurface::localVolImpl
    // (ql/termstructures/volatility/equityfx/localvolsurface.cpp), the same
    // reads and arithmetic in the same order; each QL_ENSURE returns NaN.
    using QuantLib::DiscountFactor;
    using QuantLib::Real;
    using QuantLib::Time;
    const Real nan = std::numeric_limits<Real>::quiet_NaN();
    DiscountFactor dr = riskFree_->discount(t, true);
    DiscountFactor dq = dividend_->discount(t, true);
    Real forwardValue = spot_->value() * dq / dr;

    // strike derivatives
    Real strike, y, dy, strikep, strikem;
    Real w, wp, wm, dwdy, d2wdy2;
    strike = underlyingLevel;
    y = std::log(strike / forwardValue);
    dy = ((std::fabs(y) > 0.001) ? Real(y * 0.0001) : 0.000001);
    strikep = strike * std::exp(dy);
    strikem = strike / std::exp(dy);
    w = black_->blackVariance(t, strike, true);
    wp = black_->blackVariance(t, strikep, true);
    wm = black_->blackVariance(t, strikem, true);
    dwdy = (wp - wm) / (2.0 * dy);
    d2wdy2 = (wp - 2.0 * w + wm) / (dy * dy);

    // time derivative
    Real dt, wpt, wmt, dwdt;
    if (t == 0.0) {
        dt = 0.0001;
        DiscountFactor drpt = riskFree_->discount(t + dt, true);
        DiscountFactor dqpt = dividend_->discount(t + dt, true);
        Real strikept = strike * dr * dqpt / (drpt * dq);

        wpt = black_->blackVariance(t + dt, strikept, true);
        if (!(wpt >= w)) return nan;
        dwdt = (wpt - w) / dt;
    } else {
        dt = std::min<Time>(0.0001, t / 2.0);
        DiscountFactor drpt = riskFree_->discount(t + dt, true);
        DiscountFactor drmt = riskFree_->discount(t - dt, true);
        DiscountFactor dqpt = dividend_->discount(t + dt, true);
        DiscountFactor dqmt = dividend_->discount(t - dt, true);

        Real strikept = strike * dr * dqpt / (drpt * dq);
        Real strikemt = strike * dr * dqmt / (drmt * dq);

        wpt = black_->blackVariance(t + dt, strikept, true);
        wmt = black_->blackVariance(t - dt, strikemt, true);

        if (!(wpt >= w)) return nan;
        if (!(w >= wmt)) return nan;

        dwdt = (wpt - wmt) / (2.0 * dt);
    }

    if (dwdy == 0.0 && d2wdy2 == 0.0) { // avoid /w where w might be 0.0
        return std::sqrt(dwdt);
    } else {
        Real den1 = 1.0 - y / w * dwdy;
        Real den2 = 0.25 * (-0.25 - 1.0 / w + y * y / w / w) * dwdy * dwdy;
        Real den3 = 0.5 * d2wdy2;
        Real den = den1 + den2 + den3;
        Real result = dwdt / den;

        if (!(result >= 0.0)) return nan;

        return std::sqrt(result);
    }
}

QuantLib::Volatility EdgeClampedLocalVol::localVolImpl(QuantLib::Time t, QuantLib::Real s) const {
    if (!floor_) return inner_->localVol(clampedTime(t), clampedLevel(s, t), true);
    ++reads_;
    QuantLib::Real v;
    try {
        v = dupire(clampedTime(t), clampedLevel(s, t));
    } catch (const std::exception&) {
        v = std::numeric_limits<QuantLib::Real>::quiet_NaN();
    }
    if (std::isnan(v)) {
        ++refusals_;
        ++floored_;
        return *floor_;
    }
    if (v < *floor_) {
        ++floored_;
        return *floor_;
    }
    return v;
}

void LocalVolCache::attach(BasketAsset& asset) {
    if (asset.exactStepCapable) {
        return;
    }
    if (asset.spot.empty() || asset.dividend.empty() || asset.riskFree.empty() ||
        asset.vol.empty()) {
        QUANTRA_ERROR(
            "basket model: every spot, curve and vol handle must be linked for asset: " + asset.id);
    }
    const QuantLib::Real minStrike =
        asset.localVolStrikeRange ? asset.localVolStrikeRange->first : asset.vol->minStrike();
    const QuantLib::Real maxStrike =
        asset.localVolStrikeRange ? asset.localVolStrikeRange->second : asset.vol->maxStrike();
    if (!(minStrike < maxStrike)) {
        QUANTRA_ERROR("basket model: empty local vol strike range for asset: " + asset.id);
    }
    std::vector<QuantLib::Real> bandKey;
    for (const auto& b : asset.localVolStrikeBands) {
        bandKey.push_back(b.time);
        bandKey.push_back(b.minStrike);
        bandKey.push_back(b.maxStrike);
    }
    const Key key{
        asset.vol.currentLink().get(),
        asset.spot.currentLink().get(),
        asset.dividend.currentLink().get(),
        asset.riskFree.currentLink().get(),
        static_cast<int>(asset.localVolTime),
        minStrike,
        maxStrike,
        bandKey,
        asset.localVolFloor ? *asset.localVolFloor : 0.0};
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        // The very object GeneralizedBlackScholesProcess::localVolatility()
        // would build (ql/processes/blackscholesprocess.cpp): LocalVolSurface
        // on the Black vol, the risk-free and dividend curves and the spot;
        // read at the level clamped to the interior of the strike grid and,
        // with FlatLocalVol, at the time clamped below the last expiry.
        auto inner = std::make_shared<QuantLib::LocalVolSurface>(
            asset.vol, asset.riskFree, asset.dividend, asset.spot);
        const QuantLib::Time maxTime = asset.localVolTime == LocalVolTimeExtrapolation::FlatLocalVol
                                           ? asset.vol->maxTime()
                                           : QuantLib::Null<QuantLib::Time>();
        auto clamped =
            asset.localVolStrikeBands.empty()
                ? std::make_shared<EdgeClampedLocalVol>(inner, minStrike, maxStrike, maxTime)
                : std::make_shared<EdgeClampedLocalVol>(inner, asset.localVolStrikeBands, maxTime);
        if (asset.localVolFloor) {
            clamped->setFloor(
                *asset.localVolFloor, asset.vol, asset.riskFree, asset.dividend, asset.spot);
        }
        it = cache_.emplace(key, QuantLib::Handle<QuantLib::LocalVolTermStructure>(clamped)).first;
    }
    asset.localVol = it->second;
}

bool BasketModel::allExactStepCapable(const std::vector<BasketAsset>& assets) {
    for (const auto& a : assets) {
        if (!a.exactStepCapable) {
            return false;
        }
    }
    return true;
}

LognormalBasket::LognormalBasket(
    std::vector<BasketAsset> assets,
    const std::vector<std::shared_ptr<QuantLib::StochasticProcess1D>>& processes,
    const QuantLib::Matrix& correlation)
    : QuantLib::StochasticProcessArray(processes, correlation), assets_(std::move(assets)),
      exactStep_(BasketModel::allExactStepCapable(assets_)) {}

LognormalStepTable LognormalBasket::stepTable(const QuantLib::TimeGrid& grid) const {
    if (!exactStep_) {
        QUANTRA_ERROR("basket model: the exact-step table needs every asset exact-step capable");
    }
    if (grid.size() < 2) {
        QUANTRA_ERROR("basket model: the exact-step table needs a grid with at least one step");
    }
    using QuantLib::Continuous;
    using QuantLib::NoFrequency;
    using QuantLib::Real;
    using QuantLib::Size;
    using QuantLib::Time;

    LognormalStepTable table;
    table.assets = assets_.size();
    table.steps = grid.size() - 1;
    table.drift.assign(table.assets * table.steps, 0.0);
    table.stdDev.assign(table.assets * table.steps, 0.0);
    // The matrix StochasticProcessArray correlates the draws with (its
    // constructor's pseudoSqrt(correlation, SalvagingAlgorithm::Spectral)).
    table.sqrtCorrelation = sqrtCorrelation_;
    table.x0.reserve(table.assets);
    for (const auto& a : assets_) {
        table.x0.push_back(a.spot->value());
    }
    for (Size s = 0; s < table.steps; ++s) {
        // MultiPathGenerator::next passes t0 = grid[s] and dt = grid.dt(s) to
        // evolve, which reads the term structures at t0 and t0 + dt (not at
        // grid[s + 1], which can differ in the last bit).
        const Time t0 = grid[s];
        const Time dt = grid.dt(s);
        for (Size k = 0; k < table.assets; ++k) {
            const auto& a = assets_[k];
            // Same reads, same expression, same order as
            // GeneralizedBlackScholesProcess::evolve for a strike-independent
            // vol (ql/processes/blackscholesprocess.cpp, QuantLib 1.44).
            const Real var = a.vol->blackVariance(t0 + dt, 0.01) - a.vol->blackVariance(t0, 0.01);
            const Real drift =
                (a.riskFree->forwardRate(t0, t0 + dt, Continuous, NoFrequency, true).rate() -
                 a.dividend->forwardRate(t0, t0 + dt, Continuous, NoFrequency, true).rate()) *
                    dt -
                0.5 * var;
            table.drift[s * table.assets + k] = drift;
            table.stdDev[s * table.assets + k] = std::sqrt(var);
        }
    }
    return table;
}

std::shared_ptr<LognormalBasket> BasketModel::build(
    const std::vector<BasketAsset>& assets, const QuantLib::Matrix& correlation) {
    if (assets.empty()) {
        QUANTRA_INVALID_ARGUMENT("basket model: at least one asset is required");
    }
    if (correlation.rows() != correlation.columns()) {
        std::ostringstream msg;
        msg << "basket model: correlation matrix must be square, got " << correlation.rows() << "x"
            << correlation.columns();
        QUANTRA_INVALID_ARGUMENT(msg.str());
    }
    if (correlation.rows() != assets.size()) {
        std::ostringstream msg;
        msg << "basket model: correlation matrix dimension " << correlation.rows()
            << " does not match the number of assets " << assets.size();
        QUANTRA_INVALID_ARGUMENT(msg.str());
    }

    std::set<std::string> ids;
    std::vector<std::shared_ptr<QuantLib::StochasticProcess1D>> processes;
    processes.reserve(assets.size());
    for (const auto& a : assets) {
        if (!ids.insert(a.id).second) {
            QUANTRA_INVALID_ARGUMENT("basket model: duplicate asset id: " + a.id);
        }
        if (a.spot.empty() || a.dividend.empty() || a.riskFree.empty() || a.vol.empty()) {
            QUANTRA_ERROR(
                "basket model: every spot, curve and vol handle must be linked for asset: " + a.id);
        }
        if (!(a.spot->value() > 0.0)) {
            QUANTRA_INVALID_ARGUMENT("basket model: spot must be positive for asset: " + a.id);
        }
        if (a.exactStepCapable != IsExactStepCapable(a.vol)) {
            QUANTRA_ERROR(
                "basket model: exactStepCapable does not match the vol type for asset: " + a.id);
        }
        if (a.localVol.empty()) {
            processes.push_back(
                std::make_shared<QuantLib::GeneralizedBlackScholesProcess>(
                    a.spot, a.dividend, a.riskFree, a.vol));
        } else {
            if (a.exactStepCapable) {
                QUANTRA_ERROR("basket model: a shared local vol on an exact-step asset: " + a.id);
            }
            // QuantLib's external-local-vol process: the same Euler step on
            // the same LocalVolSurface arithmetic as the lazily derived one.
            processes.push_back(
                std::make_shared<QuantLib::GeneralizedBlackScholesProcess>(
                    a.spot, a.dividend, a.riskFree, a.vol, a.localVol));
        }
    }
    return std::make_shared<LognormalBasket>(assets, processes, correlation);
}

}} // namespace quantra::mc
