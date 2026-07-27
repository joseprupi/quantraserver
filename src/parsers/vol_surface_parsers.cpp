/**
 * Vol Surface Parsers Implementation
 * 
 * All vol parsers in one file for simpler build integration.
 */

#include "vol_surface_parsers.h"

#include "require_field.h"

#include <ql/termstructures/volatility/swaption/swaptionvolmatrix.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancecurve.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancesurface.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/exercise.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/interpolatedsmilesection.hpp>
#include <ql/termstructures/volatility/sabr.hpp>
#include <ql/termstructures/volatility/sabrsmilesection.hpp>
#include <ql/termstructures/volatility/swaption/sabrswaptionvolatilitycube.hpp>
#include <ql/math/interpolations/linearinterpolation.hpp>
#include <ql/math/interpolations/bilinearinterpolation.hpp>
#include <ql/math/interpolations/bicubicsplineinterpolation.hpp>
#include <ql/math/matrix.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/math/optimization/endcriteria.hpp>

#include "sabr_calibrate_cache.h"
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <iostream>
#include <sstream>
#include <cctype>
#include "date_convert.h"

namespace quantra {

// =============================================================================
// Utility Functions
// =============================================================================

QuantLib::VolatilityType toQlVolType(quantra::enums::VolatilityType t) {
    switch (t) {
        case quantra::enums::VolatilityType_Normal:
            return QuantLib::Normal;
            
        // QuantLib encodes Black lognormal vols under ShiftedLognormal;
        // pure lognormal is represented by displacement == 0.
        case quantra::enums::VolatilityType_Lognormal:
        case quantra::enums::VolatilityType_ShiftedLognormal:
            return QuantLib::ShiftedLognormal;
            
        default:
            QUANTRA_INVALID_ARGUMENT(
                "IrVolBaseSpec.volatility_type is not a known volatility type: " +
                std::to_string(static_cast<int>(t)));
    }
    return QuantLib::ShiftedLognormal; // Unreachable, but suppresses warning
}

// =============================================================================
// Validation Helpers
// =============================================================================

namespace {

bool isBlankString(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

/// Read the presence-required quotation convention off an IrVolBaseSpec. An
/// omitted value must not fall through to the alphabetical-0 enum (Normal),
/// which would price a lognormal-quoted surface as normal vols.
quantra::enums::VolatilityType requiredVolType(const quantra::IrVolBaseSpec* b, const std::string& id) {
    if (!b || !b->volatility_type().has_value()) {
        QUANTRA_INVALID_ARGUMENT("IrVolBaseSpec.volatility_type is required for vol id: " + id);
    }
    return b->volatility_type().value();
}

void validateIrVolBaseCommon(const quantra::IrVolBaseSpec* b, const std::string& id) {
    if (!b) {
        QUANTRA_INVALID_ARGUMENT("IrVolBaseSpec missing for vol id: " + id);
    }
    if (!b->reference_date()) {
        QUANTRA_INVALID_ARGUMENT("reference_date required for vol id: " + id);
    }
    if (!b->calendar().has_value()) {
        QUANTRA_INVALID_ARGUMENT("IrVolBaseSpec.calendar is required for vol id: " + id);
    }
    if (!b->business_day_convention().has_value()) {
        QUANTRA_INVALID_ARGUMENT("IrVolBaseSpec.business_day_convention is required for vol id: " + id);
    }
    if (!b->day_counter().has_value()) {
        QUANTRA_INVALID_ARGUMENT("IrVolBaseSpec.day_counter is required for vol id: " + id);
    }

    auto volType = requiredVolType(b, id);
    double disp = b->displacement();
    
    if (volType == quantra::enums::VolatilityType_ShiftedLognormal && disp <= 0.0) {
        QUANTRA_INVALID_ARGUMENT("ShiftedLognormal requires displacement > 0 for vol id: " + id);
    }
    if (volType == quantra::enums::VolatilityType_Lognormal && disp != 0.0) {
        QUANTRA_INVALID_ARGUMENT("Lognormal requires displacement == 0 for vol id: " + id);
    }
}

void validateIrVolBaseConstant(const quantra::IrVolBaseSpec* b, const std::string& id) {
    validateIrVolBaseCommon(b, id);
    bool hasQuote = b->quote_id() && !b->quote_id()->str().empty();
    if (!hasQuote && b->constant_vol() <= 0.0) {
        QUANTRA_INVALID_ARGUMENT("constant_vol must be > 0 (or quote_id provided) for vol id: " + id);
    }
}

void validateSupportedInterpolator(quantra::enums::Interpolator interp, const std::string& label, const std::string& id) {
    if (interp != quantra::enums::Interpolator_Linear) {
        QUANTRA_INVALID_ARGUMENT(label + " only supports Linear interpolator for vol id: " + id);
    }
}

void validateBlackVolBase(const quantra::BlackVolBaseSpec* b, const std::string& id) {
    if (!b) {
        QUANTRA_INVALID_ARGUMENT("BlackVolBaseSpec missing for vol id: " + id);
    }
    if (!b->reference_date()) {
        QUANTRA_INVALID_ARGUMENT("reference_date required for vol id: " + id);
    }
    if (!b->calendar().has_value()) {
        QUANTRA_INVALID_ARGUMENT("BlackVolBaseSpec.calendar is required for vol id: " + id);
    }
    if (!b->business_day_convention().has_value()) {
        QUANTRA_INVALID_ARGUMENT("BlackVolBaseSpec.business_day_convention is required for vol id: " + id);
    }
    if (!b->day_counter().has_value()) {
        QUANTRA_INVALID_ARGUMENT("BlackVolBaseSpec.day_counter is required for vol id: " + id);
    }
    switch (b->shape()) {
        case quantra::enums::VolSurfaceShape_Constant:
        case quantra::enums::VolSurfaceShape_AtmMatrix2D:
        case quantra::enums::VolSurfaceShape_SmileCube3D:
        case quantra::enums::VolSurfaceShape_SurfaceFromPrices:
            break;
        default:
            QUANTRA_INVALID_ARGUMENT(
                "BlackVolSpec supports shape=Constant, AtmMatrix2D, SmileCube3D, SurfaceFromPrices for vol id: " + id);
    }
}

QuantLib::Period toQlPeriod(const quantra::Period* p) {
    return requirePeriod(p, "Period");
}

double resolveMatrixValue(
    const quantra::QuoteMatrix2D* m,
    int idx,
    const QuoteRegistry* quotes,
    const std::string& id) {
    double inlineValue = m->values()->Get(idx);
    if (m->quote_ids()) {
        auto* s = m->quote_ids()->Get(idx);
        if (s && s->size() > 0) {
            if (!quotes) {
                QUANTRA_ERROR("QuoteMatrix2D has quote_ids but QuoteRegistry is unavailable for vol id: " + id);
            }
            return quotes->getValue(s->str(), quantra::QuoteType_Volatility);
        }
    }
    return inlineValue;
}

double resolveTensorValue(
    const quantra::QuoteTensor3D* t,
    int idx,
    const QuoteRegistry* quotes,
    const std::string& id) {
    double inlineValue = t->values()->Get(idx);
    if (t->quote_ids()) {
        auto* s = t->quote_ids()->Get(idx);
        if (s && s->size() > 0) {
            if (!quotes) {
                QUANTRA_ERROR("QuoteTensor3D has quote_ids but QuoteRegistry is unavailable for vol id: " + id);
            }
            return quotes->getValue(s->str(), quantra::QuoteType_Volatility);
        }
    }
    return inlineValue;
}

void validateMatrix2D(const quantra::QuoteMatrix2D* m, int nRows, int nCols, const std::string& id) {
    if (!m) {
        QUANTRA_INVALID_ARGUMENT("QuoteMatrix2D missing for vol id: " + id);
    }
    if (m->n_rows() != nRows || m->n_cols() != nCols) {
        QUANTRA_INVALID_ARGUMENT("QuoteMatrix2D dims mismatch for vol id: " + id);
    }
    int expected = nRows * nCols;
    if (!m->values() || static_cast<int>(m->values()->size()) != expected) {
        QUANTRA_INVALID_ARGUMENT("QuoteMatrix2D values length mismatch for vol id: " + id);
    }
    if (m->quote_ids() && static_cast<int>(m->quote_ids()->size()) != expected) {
        QUANTRA_INVALID_ARGUMENT("QuoteMatrix2D quote_ids length mismatch for vol id: " + id);
    }
}

void validateTensor3D(const quantra::QuoteTensor3D* t, int n1, int n2, int n3, const std::string& id) {
    if (!t) {
        QUANTRA_INVALID_ARGUMENT("QuoteTensor3D missing for vol id: " + id);
    }
    if (t->n_1() != n1 || t->n_2() != n2 || t->n_3() != n3) {
        QUANTRA_INVALID_ARGUMENT("QuoteTensor3D dims mismatch for vol id: " + id);
    }
    int expected = n1 * n2 * n3;
    if (!t->values() || static_cast<int>(t->values()->size()) != expected) {
        QUANTRA_INVALID_ARGUMENT("QuoteTensor3D values length mismatch for vol id: " + id);
    }
    if (t->quote_ids() && static_cast<int>(t->quote_ids()->size()) != expected) {
        QUANTRA_INVALID_ARGUMENT("QuoteTensor3D quote_ids length mismatch for vol id: " + id);
    }
}

double periodToTime(
    const QuantLib::Date& ref,
    const QuantLib::Calendar& cal,
    QuantLib::BusinessDayConvention bdc,
    const QuantLib::DayCounter& dc,
    const QuantLib::Period& p) {
    QuantLib::Date d = cal.advance(ref, p, bdc);
    return dc.yearFraction(ref, d);
}

class SwaptionSmileCubeCustom : public QuantLib::SwaptionVolatilityStructure {
public:
    SwaptionSmileCubeCustom(
        const QuantLib::Date& ref,
        const QuantLib::Calendar& cal,
        QuantLib::BusinessDayConvention bdc,
        const QuantLib::DayCounter& dc,
        QuantLib::VolatilityType volType,
        double displacement,
        std::vector<QuantLib::Period> expiries,
        std::vector<QuantLib::Period> tenors,
        std::vector<double> strikes,
        quantra::enums::SwaptionStrikeKind strikeKind,
        std::vector<double> atm_forwards_flat,
        std::vector<double> vols_flat)
        : QuantLib::SwaptionVolatilityStructure(ref, cal, bdc, dc),
          volType_(volType),
          displacement_(displacement),
          expiries_(std::move(expiries)),
          tenors_(std::move(tenors)),
          strikes_(std::move(strikes)),
          strikeKind_(strikeKind),
          atmForwards_(std::move(atm_forwards_flat)),
          vols_(std::move(vols_flat)) {
        enableExtrapolation();
        if (!std::is_sorted(strikes_.begin(), strikes_.end())) {
            QUANTRA_INVALID_ARGUMENT("SwaptionVolSmileCubeSpec strikes must be sorted ascending");
        }
        auto dup = std::adjacent_find(strikes_.begin(), strikes_.end(), [](double a, double b) { return a >= b; });
        if (dup != strikes_.end()) {
            QUANTRA_INVALID_ARGUMENT("SwaptionVolSmileCubeSpec strikes must be strictly increasing");
        }
        if (!std::is_sorted(expiries_.begin(), expiries_.end())) {
            QUANTRA_INVALID_ARGUMENT("SwaptionVolSmileCubeSpec expiries must be sorted ascending");
        }
        auto expDup =
            std::adjacent_find(expiries_.begin(), expiries_.end(), [](const QuantLib::Period& a, const QuantLib::Period& b) {
                return !(a < b);
            });
        if (expDup != expiries_.end()) {
            QUANTRA_INVALID_ARGUMENT("SwaptionVolSmileCubeSpec expiries must be strictly increasing");
        }
        if (!std::is_sorted(tenors_.begin(), tenors_.end())) {
            QUANTRA_INVALID_ARGUMENT("SwaptionVolSmileCubeSpec tenors must be sorted ascending");
        }
        auto tenDup =
            std::adjacent_find(tenors_.begin(), tenors_.end(), [](const QuantLib::Period& a, const QuantLib::Period& b) {
                return !(a < b);
            });
        if (tenDup != tenors_.end()) {
            QUANTRA_INVALID_ARGUMENT("SwaptionVolSmileCubeSpec tenors must be strictly increasing");
        }
        if (strikeKind_ == quantra::enums::SwaptionStrikeKind_SpreadFromATM) {
            double maxAbsSpread = 0.0;
            for (double s : strikes_) {
                maxAbsSpread = std::max(maxAbsSpread, std::fabs(s));
            }
            if (maxAbsSpread > 0.50) {
                std::cout
                    << "Warning: SpreadFromATM strike axis expects rate units (e.g. 0.0025 for 25bp)"
                    << std::endl;
            }
        }
        nExp_ = static_cast<int>(expiries_.size());
        nTen_ = static_cast<int>(tenors_.size());
        if (!atmForwards_.empty() && static_cast<int>(atmForwards_.size()) != nExp_ * nTen_) {
            QUANTRA_INVALID_ARGUMENT("SwaptionVolSmileCubeSpec atm_forwards matrix size mismatch");
        }
        tExp_.reserve(expiries_.size());
        tTen_.reserve(tenors_.size());
        for (const auto& p : expiries_) tExp_.push_back(periodToTime(ref, cal, bdc, dc, p));
        for (const auto& p : tenors_) tTen_.push_back(periodToTime(ref, cal, bdc, dc, p));
        maxSwapTenor_ = tenors_.empty() ? QuantLib::Period(0, QuantLib::Days) : tenors_.back();
        maxDate_ = ref;
        QuantLib::Date maxExerciseDate = ref;
        for (const auto& p : expiries_) {
            QuantLib::Date d = cal.advance(ref, p, bdc);
            if (d > maxExerciseDate) maxExerciseDate = d;
        }
        maxDate_ = maxExerciseDate;
        if (maxSwapTenor_.length() > 0) {
            QuantLib::Date end = cal.advance(maxExerciseDate, maxSwapTenor_, bdc);
            if (end > maxDate_) maxDate_ = end;
        }
        minStrike_ = strikes_.empty() ? 0.0 : *std::min_element(strikes_.begin(), strikes_.end());
        maxStrike_ = strikes_.empty() ? 0.0 : *std::max_element(strikes_.begin(), strikes_.end());
        if (strikeKind_ == quantra::enums::SwaptionStrikeKind_SpreadFromATM && !atmForwards_.empty() && !strikes_.empty()) {
            double minAtm = *std::min_element(atmForwards_.begin(), atmForwards_.end());
            double maxAtm = *std::max_element(atmForwards_.begin(), atmForwards_.end());
            minStrike_ = minAtm + strikes_.front();
            maxStrike_ = maxAtm + strikes_.back();
        }
        runSanityChecksIfEnabled();
    }

    QuantLib::VolatilityType volatilityType() const override { return volType_; }
    QuantLib::Date maxDate() const override { return maxDate_; }
    QuantLib::Rate minStrike() const override { return minStrike_; }
    QuantLib::Rate maxStrike() const override { return maxStrike_; }
    const QuantLib::Period& maxSwapTenor() const override { return maxSwapTenor_; }
    double atmForward(QuantLib::Time optionTime, QuantLib::Time swapLength) const { return bilinearAtm(optionTime, swapLength); }

protected:
    QuantLib::Volatility volatilityImpl(QuantLib::Time optionTime, QuantLib::Time swapLength, QuantLib::Rate strike) const override {
        return triLinear(optionTime, swapLength, strike);
    }

    QuantLib::ext::shared_ptr<QuantLib::SmileSection> smileSectionImpl(
        QuantLib::Time optionTime, QuantLib::Time swapLength) const override {
        if (strikes_.empty()) {
            QUANTRA_INVALID_ARGUMENT("Smile cube strikes are empty");
        }
        double sqrtT = std::sqrt(std::max(optionTime, 1.0e-8));
        std::vector<QuantLib::Real> stdDevs;
        stdDevs.reserve(strikes_.size());
        double atm = bilinearAtm(optionTime, swapLength);
        std::vector<double> absStrikes;
        absStrikes.reserve(strikes_.size());
        for (double s : strikes_) {
            absStrikes.push_back(strikeKind_ == quantra::enums::SwaptionStrikeKind_SpreadFromATM ? atm + s : s);
        }
        for (double k : absStrikes) {
            double v = triLinear(optionTime, swapLength, k);
            stdDevs.push_back(v * sqrtT);
        }
        double atmLevel = atm;
        return QuantLib::ext::shared_ptr<QuantLib::SmileSection>(
            new QuantLib::InterpolatedSmileSection<QuantLib::Linear>(
                optionTime, absStrikes, stdDevs, atmLevel, QuantLib::Linear(),
                dayCounter(), volatilityType(), displacement_));
    }

private:
    void runSanityChecksIfEnabled() const {
        const char* v = std::getenv("QUANTRA_SMILE_SANITY_CHECKS");
        if (!v || std::string(v) != "1") {
            return;
        }
        const double eps = 1.0e-10;
        for (int j = 0; j < nTen_; ++j) {
            for (size_t k = 0; k < strikes_.size(); ++k) {
                double prevW = std::pow(vols_[idx(0, j, k)], 2.0) * std::max(tExp_[0], eps);
                for (int i = 1; i < nExp_; ++i) {
                    double w = std::pow(vols_[idx(i, j, k)], 2.0) * std::max(tExp_[i], eps);
                    if (w + 1.0e-12 < prevW) {
                        std::cout
                            << "Warning: total variance decreases with expiry at tenorIdx=" << j
                            << ", strikeIdx=" << k << " (calendar sanity warning)"
                            << std::endl;
                        break;
                    }
                    prevW = w;
                }
            }
        }
    }

    size_t idx(size_t i, size_t j, size_t k) const {
        return (i * tenors_.size() + j) * strikes_.size() + k;
    }
    size_t idx2d(size_t i, size_t j) const {
        return i * tenors_.size() + j;
    }

    static void bracket(const std::vector<double>& grid, double x, size_t& i0, size_t& i1, double& w) {
        // v1 behavior: outside-grid values are flat-extended at nearest boundary node.
        if (grid.empty()) {
            i0 = i1 = 0;
            w = 0.0;
            return;
        }
        if (x <= grid.front()) {
            i0 = i1 = 0;
            w = 0.0;
            return;
        }
        if (x >= grid.back()) {
            i0 = i1 = grid.size() - 1;
            w = 0.0;
            return;
        }
        auto it = std::lower_bound(grid.begin(), grid.end(), x);
        size_t hi = static_cast<size_t>(it - grid.begin());
        size_t lo = hi - 1;
        i0 = lo;
        i1 = hi;
        double x0 = grid[lo], x1 = grid[hi];
        w = (x - x0) / (x1 - x0);
    }

    double nodeVolWithStrike(size_t i, size_t j, double strikeAbs) const {
        double axisX = strikeAbs;
        if (strikeKind_ == quantra::enums::SwaptionStrikeKind_SpreadFromATM) {
            axisX = strikeAbs - nodeAtm(i, j);
        }

        size_t k0, k1;
        double wk;
        bracket(strikes_, axisX, k0, k1, wk);
        double v0 = vols_[idx(i, j, k0)];
        double v1 = vols_[idx(i, j, k1)];
        return v0 * (1 - wk) + v1 * wk;
    }

    double nodeAtm(size_t i, size_t j) const {
        if (atmForwards_.empty()) {
            if (strikeKind_ == quantra::enums::SwaptionStrikeKind_SpreadFromATM) {
                QUANTRA_INVALID_ARGUMENT("SpreadFromATM smile cube requires ATM forwards (server-computed or provided)");
            }
            return strikes_.empty() ? 0.0 : strikes_[strikes_.size() / 2];
        }
        return atmForwards_[idx2d(i, j)];
    }

    double bilinearAtm(double tExp, double tTen) const {
        if (atmForwards_.empty()) {
            if (strikeKind_ == quantra::enums::SwaptionStrikeKind_SpreadFromATM) {
                QUANTRA_INVALID_ARGUMENT("SpreadFromATM smile cube requires ATM forwards (server-computed or provided)");
            }
            // Absolute-strike cube: this is an interpolation anchor, not a market ATM.
            return strikes_.empty() ? 0.0 : strikes_[strikes_.size() / 2];
        }
        size_t e0, e1, t0, t1;
        double we, wt;
        bracket(tExp_, tExp, e0, e1, we);
        bracket(tTen_, tTen, t0, t1, wt);

        double a00 = nodeAtm(e0, t0);
        double a01 = nodeAtm(e0, t1);
        double a10 = nodeAtm(e1, t0);
        double a11 = nodeAtm(e1, t1);
        double a0 = a00 * (1 - wt) + a01 * wt;
        double a1 = a10 * (1 - wt) + a11 * wt;
        return a0 * (1 - we) + a1 * we;
    }

    double triLinear(double tExp, double tTen, double strikeAbs) const {
        size_t e0, e1, t0, t1;
        double we, wt;
        bracket(tExp_, tExp, e0, e1, we);
        bracket(tTen_, tTen, t0, t1, wt);
        const double eps = 1.0e-12;
        const double tNode0 = std::max(tExp_[e0], eps);
        const double tNode1 = std::max(tExp_[e1], eps);
        const double tQuery = std::max(tExp, eps);

        // Interpolate on total variance across expiry to improve calendar behavior.
        const double w00 = std::pow(nodeVolWithStrike(e0, t0, strikeAbs), 2.0) * tNode0;
        const double w01 = std::pow(nodeVolWithStrike(e0, t1, strikeAbs), 2.0) * tNode0;
        const double w10 = std::pow(nodeVolWithStrike(e1, t0, strikeAbs), 2.0) * tNode1;
        const double w11 = std::pow(nodeVolWithStrike(e1, t1, strikeAbs), 2.0) * tNode1;

        const double w0 = w00 * (1 - wt) + w01 * wt;
        const double w1 = w10 * (1 - wt) + w11 * wt;
        const double w = w0 * (1 - we) + w1 * we;
        return std::sqrt(std::max(w, 0.0) / tQuery);
    }

    QuantLib::VolatilityType volType_;
    double displacement_;
    std::vector<QuantLib::Period> expiries_;
    std::vector<QuantLib::Period> tenors_;
    std::vector<double> strikes_;
    quantra::enums::SwaptionStrikeKind strikeKind_;
    std::vector<double> atmForwards_;
    std::vector<double> tExp_;
    std::vector<double> tTen_;
    std::vector<double> vols_;
    int nExp_ = 0;
    int nTen_ = 0;
    QuantLib::Date maxDate_;
    QuantLib::Rate minStrike_ = 0.0;
    QuantLib::Rate maxStrike_ = 0.0;
    QuantLib::Period maxSwapTenor_;
};

// SABR-params swaption vol structure built from per-node (alpha, beta, nu, rho)
// and per-node forwards. Smile evaluation at each node delegates to QuantLib's
// SabrSmileSection (Hagan formula). Cross-node interpolation: total variance
// (sigma^2 * t) bilinearly across (expiry, tenor) — same convention as the
// existing SwaptionSmileCubeCustom so the two cube types behave consistently.
class SwaptionSabrParamsCube : public QuantLib::SwaptionVolatilityStructure {
public:
    SwaptionSabrParamsCube(
        const QuantLib::Date& ref,
        const QuantLib::Calendar& cal,
        QuantLib::BusinessDayConvention bdc,
        const QuantLib::DayCounter& dc,
        QuantLib::VolatilityType volType,
        double displacement,
        std::vector<QuantLib::Period> expiries,
        std::vector<QuantLib::Period> tenors,
        std::vector<double> alpha,
        std::vector<double> beta,
        std::vector<double> rho,
        std::vector<double> nu,
        std::vector<double> atmForwardsFlat)
        : QuantLib::SwaptionVolatilityStructure(ref, cal, bdc, dc),
          volType_(volType),
          displacement_(displacement),
          expiries_(std::move(expiries)),
          tenors_(std::move(tenors)),
          alpha_(std::move(alpha)),
          beta_(std::move(beta)),
          rho_(std::move(rho)),
          nu_(std::move(nu)),
          atmForwards_(std::move(atmForwardsFlat)) {
        enableExtrapolation();
        if (expiries_.empty() || tenors_.empty()) {
            QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec expiries/tenors must be non-empty");
        }
        if (!std::is_sorted(expiries_.begin(), expiries_.end())) {
            QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec expiries must be sorted ascending");
        }
        auto expDup = std::adjacent_find(
            expiries_.begin(), expiries_.end(),
            [](const QuantLib::Period& a, const QuantLib::Period& b) { return !(a < b); });
        if (expDup != expiries_.end()) {
            QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec expiries must be strictly increasing");
        }
        if (!std::is_sorted(tenors_.begin(), tenors_.end())) {
            QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec tenors must be sorted ascending");
        }
        auto tenDup = std::adjacent_find(
            tenors_.begin(), tenors_.end(),
            [](const QuantLib::Period& a, const QuantLib::Period& b) { return !(a < b); });
        if (tenDup != tenors_.end()) {
            QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec tenors must be strictly increasing");
        }
        nExp_ = static_cast<int>(expiries_.size());
        nTen_ = static_cast<int>(tenors_.size());
        const int expected = nExp_ * nTen_;
        if (static_cast<int>(alpha_.size()) != expected ||
            static_cast<int>(beta_.size()) != expected ||
            static_cast<int>(rho_.size()) != expected ||
            static_cast<int>(nu_.size()) != expected) {
            QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec parameter grid sizes must equal nExp * nTen");
        }
        if (static_cast<int>(atmForwards_.size()) != expected) {
            QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec ATM forwards grid size must equal nExp * nTen");
        }

        tExp_.reserve(expiries_.size());
        tTen_.reserve(tenors_.size());
        for (const auto& p : expiries_) tExp_.push_back(periodToTime(ref, cal, bdc, dc, p));
        for (const auto& p : tenors_) tTen_.push_back(periodToTime(ref, cal, bdc, dc, p));

        smiles_.resize(static_cast<size_t>(expected));
        for (int i = 0; i < nExp_; ++i) {
            for (int j = 0; j < nTen_; ++j) {
                const size_t k = idx2d(i, j);
                const double t = std::max(tExp_[i], 1.0e-8);
                const double f = atmForwards_[k];
                if (!std::isfinite(f) || f + displacement_ <= 0.0) {
                    QUANTRA_INVALID_ARGUMENT(
                        "SwaptionSabrParamsSpec requires positive (forward + displacement) at every node");
                }
                std::vector<QuantLib::Real> sabrParams(4);
                sabrParams[0] = alpha_[k];
                sabrParams[1] = beta_[k];
                sabrParams[2] = nu_[k];
                sabrParams[3] = rho_[k];
                smiles_[k] = QuantLib::ext::shared_ptr<QuantLib::SabrSmileSection>(
                    new QuantLib::SabrSmileSection(
                        t, f, sabrParams, displacement_, volType_));
            }
        }

        maxSwapTenor_ = tenors_.back();
        QuantLib::Date maxExerciseDate = ref;
        for (const auto& p : expiries_) {
            QuantLib::Date d = cal.advance(ref, p, bdc);
            if (d > maxExerciseDate) maxExerciseDate = d;
        }
        maxDate_ = maxExerciseDate;
        if (maxSwapTenor_.length() > 0) {
            QuantLib::Date end = cal.advance(maxExerciseDate, maxSwapTenor_, bdc);
            if (end > maxDate_) maxDate_ = end;
        }
        // Strike support is unbounded above; lower bound is -displacement_
        // (consistent with QuantLib::SabrSmileSection::minStrike()).
        minStrike_ = -displacement_;
        maxStrike_ = QL_MAX_REAL;
    }

    QuantLib::VolatilityType volatilityType() const override { return volType_; }
    QuantLib::Date maxDate() const override { return maxDate_; }
    QuantLib::Rate minStrike() const override { return minStrike_; }
    QuantLib::Rate maxStrike() const override { return maxStrike_; }
    const QuantLib::Period& maxSwapTenor() const override { return maxSwapTenor_; }

protected:
    QuantLib::Volatility volatilityImpl(
        QuantLib::Time optionTime, QuantLib::Time swapLength, QuantLib::Rate strike) const override {
        return blendedVol(optionTime, swapLength, strike);
    }

    QuantLib::ext::shared_ptr<QuantLib::SmileSection> smileSectionImpl(
        QuantLib::Time optionTime, QuantLib::Time swapLength) const override {
        // Sample the blended (alpha,beta,nu,rho)-implied vol at a small grid of
        // strikes around the bracketed-node ATM forward, then wrap as an
        // InterpolatedSmileSection. Mirrors SwaptionSmileCubeCustom's pattern
        // so consumers get a real SmileSection without exposing the per-node
        // Sabr internals.
        size_t e0, e1, t0, t1;
        double we, wt;
        bracket(tExp_, optionTime, e0, e1, we);
        bracket(tTen_, swapLength, t0, t1, wt);
        const double f00 = atmForwards_[idx2d(static_cast<int>(e0), static_cast<int>(t0))];
        const double f01 = atmForwards_[idx2d(static_cast<int>(e0), static_cast<int>(t1))];
        const double f10 = atmForwards_[idx2d(static_cast<int>(e1), static_cast<int>(t0))];
        const double f11 = atmForwards_[idx2d(static_cast<int>(e1), static_cast<int>(t1))];
        const double f0 = f00 * (1.0 - wt) + f01 * wt;
        const double f1 = f10 * (1.0 - wt) + f11 * wt;
        const double atm = f0 * (1.0 - we) + f1 * we;

        const double t = std::max(optionTime, 1.0e-8);
        const double sqrtT = std::sqrt(t);
        // Strike grid: ATM and a moderate spread around it. Consumers wanting
        // off-grid strikes can call volatility(...) directly; this section is
        // primarily an interface helper.
        std::vector<double> spreads = {-0.02, -0.01, -0.005, -0.0025, 0.0, 0.0025, 0.005, 0.01, 0.02};
        std::vector<double> absStrikes;
        absStrikes.reserve(spreads.size());
        std::vector<QuantLib::Real> stdDevs;
        stdDevs.reserve(spreads.size());
        for (double s : spreads) {
            double k = atm + s;
            if (k + displacement_ <= 0.0) continue;
            double v = blendedVol(optionTime, swapLength, k);
            absStrikes.push_back(k);
            stdDevs.push_back(v * sqrtT);
        }
        if (absStrikes.size() < 2) {
            // Fallback: return the bracketed-node section directly.
            return smiles_[idx2d(static_cast<int>(e0), static_cast<int>(t0))];
        }
        return QuantLib::ext::shared_ptr<QuantLib::SmileSection>(
            new QuantLib::InterpolatedSmileSection<QuantLib::Linear>(
                t, absStrikes, stdDevs, atm, QuantLib::Linear(),
                dayCounter(), volatilityType(), displacement_));
    }

private:
    size_t idx2d(int i, int j) const {
        return static_cast<size_t>(i) * tenors_.size() + static_cast<size_t>(j);
    }

    static void bracket(const std::vector<double>& grid, double x, size_t& i0, size_t& i1, double& w) {
        if (grid.empty()) { i0 = i1 = 0; w = 0.0; return; }
        if (x <= grid.front()) { i0 = i1 = 0; w = 0.0; return; }
        if (x >= grid.back()) { i0 = i1 = grid.size() - 1; w = 0.0; return; }
        auto it = std::lower_bound(grid.begin(), grid.end(), x);
        size_t hi = static_cast<size_t>(it - grid.begin());
        size_t lo = hi - 1;
        i0 = lo; i1 = hi;
        w = (x - grid[lo]) / (grid[hi] - grid[lo]);
    }

    double nodeVol(size_t i, size_t j, double strike) const {
        return smiles_[idx2d(static_cast<int>(i), static_cast<int>(j))]->volatility(strike);
    }

    double blendedVol(double optionTime, double swapLength, double strike) const {
        size_t e0, e1, t0, t1;
        double we, wt;
        bracket(tExp_, optionTime, e0, e1, we);
        bracket(tTen_, swapLength, t0, t1, wt);

        const double eps = 1.0e-12;
        const double tNode0 = std::max(tExp_[e0], eps);
        const double tNode1 = std::max(tExp_[e1], eps);
        const double tQuery = std::max(optionTime, eps);

        const double v00 = nodeVol(e0, t0, strike);
        const double v01 = nodeVol(e0, t1, strike);
        const double v10 = nodeVol(e1, t0, strike);
        const double v11 = nodeVol(e1, t1, strike);

        // Total variance interpolation across expiry; linear across tenor.
        const double w00 = v00 * v00 * tNode0;
        const double w01 = v01 * v01 * tNode0;
        const double w10 = v10 * v10 * tNode1;
        const double w11 = v11 * v11 * tNode1;
        const double w0 = w00 * (1.0 - wt) + w01 * wt;
        const double w1 = w10 * (1.0 - wt) + w11 * wt;
        const double w = w0 * (1.0 - we) + w1 * we;
        return std::sqrt(std::max(w, 0.0) / tQuery);
    }

    QuantLib::VolatilityType volType_;
    double displacement_;
    std::vector<QuantLib::Period> expiries_;
    std::vector<QuantLib::Period> tenors_;
    std::vector<double> alpha_;
    std::vector<double> beta_;
    std::vector<double> rho_;
    std::vector<double> nu_;
    std::vector<double> atmForwards_;
    std::vector<double> tExp_;
    std::vector<double> tTen_;
    std::vector<QuantLib::ext::shared_ptr<QuantLib::SabrSmileSection>> smiles_;
    int nExp_ = 0;
    int nTen_ = 0;
    QuantLib::Date maxDate_;
    QuantLib::Rate minStrike_ = 0.0;
    QuantLib::Rate maxStrike_ = 0.0;
    QuantLib::Period maxSwapTenor_;
};

double resolveVolValue(
    double inlineValue,
    const flatbuffers::String* quoteId,
    const QuoteRegistry* quotes,
    const std::string& id) {
    if (quoteId) {
        const std::string qid = quoteId->str();
        if (!qid.empty()) {
            if (!quotes) {
                QUANTRA_ERROR("quote_id provided but QuoteRegistry is unavailable for vol id: " + id);
            }
            return quotes->getValue(qid, quantra::QuoteType_Volatility);
        }
    }
    if (inlineValue <= 0.0) {
        QUANTRA_INVALID_ARGUMENT("constant_vol must be > 0 for vol id: " + id);
    }
    return inlineValue;
}

double resolveMatrixValueAnyType(
    const quantra::QuoteMatrix2D* m,
    int idx,
    const QuoteRegistry* quotes,
    const std::string& id,
    const std::string& label) {
    double inlineValue = m->values()->Get(idx);
    if (m->quote_ids()) {
        auto* s = m->quote_ids()->Get(idx);
        if (s && s->size() > 0) {
            if (!quotes) {
                QUANTRA_ERROR(
                    label + " has quote_ids but QuoteRegistry is unavailable for vol id: " + id);
            }
            auto qh = quotes->getHandle(s->str());
            return qh->value();
        }
    }
    return inlineValue;
}

bool hasNonEmptyPeriods(const flatbuffers::Vector<flatbuffers::Offset<quantra::Period>>* v) {
    return v && v->size() > 0;
}

bool hasNonEmptyReals(const flatbuffers::Vector<double>* v) {
    return v && v->size() > 0;
}

bool hasNonEmptyStrings(const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>* v) {
    return v && v->size() > 0;
}

bool hasMatrixData(const quantra::QuoteMatrix2D* m) {
    return m && m->values() && m->values()->size() > 0;
}

bool hasNonEmptyString(const flatbuffers::String* s) {
    return s && !s->str().empty();
}

enum class SurfaceInterpolationMode {
    Bilinear,
    Bicubic
};

SurfaceInterpolationMode resolveBlackSurfaceInterpolation(
    const quantra::BlackVolSpec* payload,
    const std::string& id) {
    const auto expiryInterp = payload->expiry_interpolator();
    const auto strikeInterp = payload->strike_interpolator();
    const bool legacyExpiryKnown =
        expiryInterp == quantra::enums::Interpolator_Linear ||
        expiryInterp == quantra::enums::Interpolator_LogCubic;
    const bool legacyStrikeKnown =
        strikeInterp == quantra::enums::Interpolator_Linear ||
        strikeInterp == quantra::enums::Interpolator_LogCubic;
    if (!legacyExpiryKnown || !legacyStrikeKnown) {
        QUANTRA_INVALID_ARGUMENT(
            "BlackVolSpec legacy expiry/strike interpolators only support Linear or LogCubic for vol id: " + id);
    }
    if (expiryInterp != strikeInterp) {
        QUANTRA_INVALID_ARGUMENT(
            "BlackVolSpec legacy expiry/strike interpolators must match for vol id: " + id);
    }

    // Backward compatibility: legacy 1D pair explicitly selects 2D mode.
    if (expiryInterp == quantra::enums::Interpolator_LogCubic) {
        return SurfaceInterpolationMode::Bicubic;
    }
    switch (payload->surface_interpolator()) {
        case quantra::enums::SurfaceInterpolator2D_Bilinear:
            return SurfaceInterpolationMode::Bilinear;
        case quantra::enums::SurfaceInterpolator2D_Bicubic:
            return SurfaceInterpolationMode::Bicubic;
        default:
            QUANTRA_INVALID_ARGUMENT("Unsupported surface_interpolator for vol id: " + id);
    }
    return SurfaceInterpolationMode::Bilinear;
}

QuantLib::Option::Type toQlEquityOptionType(quantra::enums::EquityOptionType t, const std::string& id) {
    switch (t) {
        case quantra::enums::EquityOptionType_Call:
            return QuantLib::Option::Call;
        case quantra::enums::EquityOptionType_Put:
            return QuantLib::Option::Put;
        default:
            QUANTRA_INVALID_ARGUMENT("Unsupported equity option_type for vol id: " + id);
    }
    return QuantLib::Option::Call;
}

} // anonymous namespace

// =============================================================================
// Optionlet Vol Parser (for Caps/Floors)
// =============================================================================

OptionletVolEntry parseOptionletVol(const quantra::VolSurfaceSpec* spec, const QuoteRegistry* quotes) {
    if (!spec || !spec->id()) {
        QUANTRA_INVALID_ARGUMENT("VolSurfaceSpec or id is null");
    }
    std::string id = spec->id()->str();
    
    auto* payload = spec->payload_as_OptionletVolSpec();
    if (!payload) {
        QUANTRA_INVALID_ARGUMENT("OptionletVolSpec payload missing for vol id: " + id);
    }

    const auto* b = payload->base();
    validateIrVolBaseConstant(b, id);

    QuantLib::Date ref = DateToQL(b->reference_date()->str());
    QuantLib::Calendar cal = CalendarToQL(b->calendar().value());
    QuantLib::BusinessDayConvention bdc = ConventionToQL(b->business_day_convention().value());
    QuantLib::DayCounter dc = DayCounterToQL(b->day_counter().value());
    double vol = resolveVolValue(b->constant_vol(), b->quote_id(), quotes, id);
    double disp = b->displacement();
    QuantLib::VolatilityType qlType = toQlVolType(requiredVolType(b, id));

    auto qlVol = std::make_shared<QuantLib::ConstantOptionletVolatility>(
        ref, cal, bdc, vol, dc, qlType, disp
    );

    OptionletVolEntry entry;
    entry.handle = QuantLib::Handle<QuantLib::OptionletVolatilityStructure>(qlVol);
    entry.qlVolType = qlType;
    entry.displacement = disp;
    entry.constantVol = vol;
    entry.referenceDate = ref;
    entry.calendar = cal;
    entry.calendarFb = b->calendar().value();
    entry.businessDayConvention = bdc;
    entry.businessDayConventionFb = b->business_day_convention().value();
    entry.dayCounter = dc;
    
    return entry;
}

// =============================================================================
// Swaption Vol Parser
// =============================================================================

SwaptionVolEntry parseSwaptionVol(const quantra::VolSurfaceSpec* spec, const QuoteRegistry* quotes) {
    if (!spec || !spec->id()) {
        QUANTRA_INVALID_ARGUMENT("VolSurfaceSpec or id is null");
    }
    std::string id = spec->id()->str();

    auto* wrapper = spec->payload_as_SwaptionVolSpec();
    if (!wrapper) {
        QUANTRA_INVALID_ARGUMENT("SwaptionVolSpec payload missing for vol id: " + id);
    }
    std::string wrapperSwapIndexId = wrapper->swap_index_id() ? wrapper->swap_index_id()->str() : "";
    if (isBlankString(wrapperSwapIndexId)) {
        QUANTRA_INVALID_ARGUMENT("SwaptionVolSpec.swap_index_id is required for vol id: " + id);
    }

    switch (wrapper->payload_type()) {
        case quantra::SwaptionVolPayload_SwaptionVolConstantSpec: {
            auto* payload = wrapper->payload_as_SwaptionVolConstantSpec();
            if (!payload || !payload->base()) {
                QUANTRA_INVALID_ARGUMENT("SwaptionVolConstantSpec base missing for vol id: " + id);
            }
            const auto* b = payload->base();
            validateIrVolBaseConstant(b, id);

            QuantLib::Date ref = DateToQL(b->reference_date()->str());
            QuantLib::Calendar cal = CalendarToQL(b->calendar().value());
            QuantLib::BusinessDayConvention bdc = ConventionToQL(b->business_day_convention().value());
            QuantLib::DayCounter dc = DayCounterToQL(b->day_counter().value());
            double vol = resolveVolValue(b->constant_vol(), b->quote_id(), quotes, id);
            double disp = b->displacement();
            QuantLib::VolatilityType qlType = toQlVolType(requiredVolType(b, id));

            auto qlVol = std::make_shared<QuantLib::ConstantSwaptionVolatility>(
                ref, cal, bdc, vol, dc, qlType, disp
            );

            SwaptionVolEntry entry;
            entry.handle = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(qlVol);
            entry.qlVolType = qlType;
            entry.displacement = disp;
            entry.constantVol = vol;
            entry.referenceDate = ref;
            entry.calendar = cal;
            entry.businessDayConvention = bdc;
            entry.dayCounter = dc;
            entry.volKind = quantra::enums::SwaptionVolKind_Constant;
            entry.swapIndexId = wrapperSwapIndexId;
            return entry;
        }

        case quantra::SwaptionVolPayload_SwaptionVolAtmMatrixSpec: {
            auto* payload = wrapper->payload_as_SwaptionVolAtmMatrixSpec();
            if (!payload || !payload->base()) {
                QUANTRA_INVALID_ARGUMENT("SwaptionVolAtmMatrixSpec base missing for vol id: " + id);
            }
            const auto* b = payload->base();
            validateIrVolBaseCommon(b, id);
            validateSupportedInterpolator(payload->expiry_interpolator(), "expiry_interpolator", id);
            validateSupportedInterpolator(payload->tenor_interpolator(), "tenor_interpolator", id);

            QuantLib::Date ref = DateToQL(b->reference_date()->str());
            QuantLib::Calendar cal = CalendarToQL(b->calendar().value());
            QuantLib::BusinessDayConvention bdc = ConventionToQL(b->business_day_convention().value());
            QuantLib::DayCounter dc = DayCounterToQL(b->day_counter().value());
            double disp = b->displacement();
            QuantLib::VolatilityType qlType = toQlVolType(requiredVolType(b, id));

            if (!payload->expiries() || !payload->tenors()) {
                QUANTRA_INVALID_ARGUMENT("SwaptionVolAtmMatrixSpec expiries/tenors missing for vol id: " + id);
            }
            int nExp = static_cast<int>(payload->expiries()->size());
            int nTen = static_cast<int>(payload->tenors()->size());
            if (nExp <= 0 || nTen <= 0) {
                QUANTRA_INVALID_ARGUMENT("SwaptionVolAtmMatrixSpec expiries/tenors empty for vol id: " + id);
            }

            std::vector<QuantLib::Period> expiries;
            expiries.reserve(nExp);
            for (auto it = payload->expiries()->begin(); it != payload->expiries()->end(); ++it) {
                expiries.push_back(toQlPeriod(*it));
            }
            std::vector<QuantLib::Period> tenors;
            tenors.reserve(nTen);
            for (auto it = payload->tenors()->begin(); it != payload->tenors()->end(); ++it) {
                tenors.push_back(toQlPeriod(*it));
            }

            const auto* m = payload->vols();
            validateMatrix2D(m, nExp, nTen, id);
            QuantLib::Matrix vols(nExp, nTen);
            std::vector<double> flat;
            flat.reserve(nExp * nTen);

            for (int i = 0; i < nExp; i++) {
                for (int j = 0; j < nTen; j++) {
                    int idx = i * nTen + j;
                    double v = resolveMatrixValue(m, idx, quotes, id);
                    if (v <= 0.0) {
                        QUANTRA_INVALID_ARGUMENT("SwaptionVolAtmMatrixSpec vol must be > 0 for vol id: " + id);
                    }
                    vols[i][j] = v;
                    flat.push_back(v);
                }
            }

            QuantLib::Matrix shifts;
            if (disp != 0.0) {
                shifts = QuantLib::Matrix(nExp, nTen, disp);
            }
            auto qlVol = std::make_shared<QuantLib::SwaptionVolatilityMatrix>(
                ref, cal, bdc, expiries, tenors, vols, dc, false, qlType, shifts
            );

            SwaptionVolEntry entry;
            entry.handle = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(qlVol);
            entry.qlVolType = qlType;
            entry.displacement = disp;
            entry.referenceDate = ref;
            entry.calendar = cal;
            entry.businessDayConvention = bdc;
            entry.dayCounter = dc;
            entry.volKind = quantra::enums::SwaptionVolKind_AtmMatrix2D;
            entry.constantVol = std::numeric_limits<double>::quiet_NaN();
            entry.expiries = expiries;
            entry.tenors = tenors;
            entry.volsFlat = flat;
            entry.nExp = nExp;
            entry.nTen = nTen;
            entry.swapIndexId = wrapperSwapIndexId;
            return entry;
        }

        case quantra::SwaptionVolPayload_SwaptionVolSmileCubeSpec: {
            auto* payload = wrapper->payload_as_SwaptionVolSmileCubeSpec();
            if (!payload || !payload->base()) {
                QUANTRA_INVALID_ARGUMENT("SwaptionVolSmileCubeSpec base missing for vol id: " + id);
            }
            const auto* b = payload->base();
            validateIrVolBaseCommon(b, id);
            validateSupportedInterpolator(payload->expiry_interpolator(), "expiry_interpolator", id);
            validateSupportedInterpolator(payload->tenor_interpolator(), "tenor_interpolator", id);
            validateSupportedInterpolator(payload->strike_interpolator(), "strike_interpolator", id);

            QuantLib::Date ref = DateToQL(b->reference_date()->str());
            QuantLib::Calendar cal = CalendarToQL(b->calendar().value());
            QuantLib::BusinessDayConvention bdc = ConventionToQL(b->business_day_convention().value());
            QuantLib::DayCounter dc = DayCounterToQL(b->day_counter().value());
            double disp = b->displacement();
            QuantLib::VolatilityType qlType = toQlVolType(requiredVolType(b, id));

            if (!payload->expiries() || !payload->tenors() || !payload->strikes()) {
                QUANTRA_INVALID_ARGUMENT("SwaptionVolSmileCubeSpec grids missing for vol id: " + id);
            }
            int nExp = static_cast<int>(payload->expiries()->size());
            int nTen = static_cast<int>(payload->tenors()->size());
            int nStr = static_cast<int>(payload->strikes()->size());
            if (nExp <= 0 || nTen <= 0 || nStr <= 0) {
                QUANTRA_INVALID_ARGUMENT("SwaptionVolSmileCubeSpec grids empty for vol id: " + id);
            }

            std::vector<QuantLib::Period> expiries;
            expiries.reserve(nExp);
            for (auto it = payload->expiries()->begin(); it != payload->expiries()->end(); ++it) {
                expiries.push_back(toQlPeriod(*it));
            }
            std::vector<QuantLib::Period> tenors;
            tenors.reserve(nTen);
            for (auto it = payload->tenors()->begin(); it != payload->tenors()->end(); ++it) {
                tenors.push_back(toQlPeriod(*it));
            }
            std::vector<double> strikes;
            strikes.reserve(nStr);
            for (auto it = payload->strikes()->begin(); it != payload->strikes()->end(); ++it) {
                strikes.push_back(*it);
            }
            bool allowExternalAtm = payload->allow_external_atm();
            auto strikeKind = payload->strike_kind();
            std::vector<double> atmForwards;
            if (payload->atm_forwards()) {
                if (!allowExternalAtm) {
                    QUANTRA_INVALID_ARGUMENT(
                        "SwaptionVolSmileCubeSpec atm_forwards requires allow_external_atm=true for vol id: " + id);
                }
                const auto* atm = payload->atm_forwards();
                validateMatrix2D(atm, nExp, nTen, id);
                atmForwards.reserve(nExp * nTen);
                for (int idx = 0; idx < nExp * nTen; ++idx) {
                    atmForwards.push_back(resolveMatrixValue(atm, idx, quotes, id));
                }
            }
            if (strikeKind == quantra::enums::SwaptionStrikeKind_Absolute && !atmForwards.empty()) {
                QUANTRA_INVALID_ARGUMENT(
                    "SwaptionVolSmileCubeSpec atm_forwards is not allowed when strike_kind=Absolute for vol id: " + id);
            }
            const auto* t = payload->vols();
            validateTensor3D(t, nExp, nTen, nStr, id);
            int expected = nExp * nTen * nStr;
            std::vector<double> vols;
            vols.reserve(expected);
            for (int idx = 0; idx < expected; idx++) {
                double v = resolveTensorValue(t, idx, quotes, id);
                if (v <= 0.0) {
                    QUANTRA_INVALID_ARGUMENT("SwaptionVolSmileCubeSpec vol must be > 0 for vol id: " + id);
                }
                vols.push_back(v);
            }

            SwaptionVolEntry entry;
            // For SpreadFromATM without external ATM, defer handle construction until
            // finalizeSwaptionVolEntryForPricing injects server-computed ATM forwards.
            if (!(strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM && atmForwards.empty())) {
                auto qlVol = std::make_shared<SwaptionSmileCubeCustom>(
                    ref, cal, bdc, dc, qlType, disp, expiries, tenors, strikes, strikeKind, atmForwards, vols);
                entry.handle = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(qlVol);
            }
            entry.qlVolType = qlType;
            entry.displacement = disp;
            entry.referenceDate = ref;
            entry.calendar = cal;
            entry.businessDayConvention = bdc;
            entry.dayCounter = dc;
            entry.volKind = quantra::enums::SwaptionVolKind_SmileCube3D;
            entry.constantVol = std::numeric_limits<double>::quiet_NaN();
            entry.expiries = expiries;
            entry.tenors = tenors;
            entry.strikes = strikes;
            entry.swapIndexId = wrapperSwapIndexId;
            entry.strikeKind = strikeKind;
            entry.allowExternalAtm = allowExternalAtm;
            entry.atmForwardsFlat = atmForwards;
            entry.volsFlat = vols;
            entry.nExp = nExp;
            entry.nTen = nTen;
            entry.nStrikes = nStr;
            return entry;
        }

        case quantra::SwaptionVolPayload_SwaptionSabrParamsSpec: {
            auto* payload = wrapper->payload_as_SwaptionSabrParamsSpec();
            if (!payload || !payload->base()) {
                QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec base missing for vol id: " + id);
            }
            const auto* b = payload->base();
            validateIrVolBaseCommon(b, id);

            // SABR via Hagan returns lognormal Black vol (or shifted lognormal
            // when displacement > 0). Normal SABR is a separate model with its
            // own engine pairing rules; reject for v1 rather than producing
            // wrong vols silently.
            if (requiredVolType(b, id) == quantra::enums::VolatilityType_Normal) {
                QUANTRA_INVALID_ARGUMENT(
                    "SwaptionSabrParamsSpec only supports Lognormal/ShiftedLognormal vol type "
                    "(Normal SABR is intentionally not supported for v1) for vol id: " + id);
            }

            QuantLib::Date ref = DateToQL(b->reference_date()->str());
            QuantLib::Calendar cal = CalendarToQL(b->calendar().value());
            QuantLib::BusinessDayConvention bdc = ConventionToQL(b->business_day_convention().value());
            QuantLib::DayCounter dc = DayCounterToQL(b->day_counter().value());
            double disp = b->displacement();
            QuantLib::VolatilityType qlType = toQlVolType(requiredVolType(b, id));

            if (!payload->expiries() || !payload->tenors()) {
                QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec expiries/tenors missing for vol id: " + id);
            }
            int nExp = static_cast<int>(payload->expiries()->size());
            int nTen = static_cast<int>(payload->tenors()->size());
            if (nExp <= 0 || nTen <= 0) {
                QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec expiries/tenors empty for vol id: " + id);
            }

            std::vector<QuantLib::Period> expiries;
            expiries.reserve(nExp);
            for (auto it = payload->expiries()->begin(); it != payload->expiries()->end(); ++it) {
                expiries.push_back(toQlPeriod(*it));
            }
            std::vector<QuantLib::Period> tenors;
            tenors.reserve(nTen);
            for (auto it = payload->tenors()->begin(); it != payload->tenors()->end(); ++it) {
                tenors.push_back(toQlPeriod(*it));
            }

            const auto* mAlpha = payload->alpha();
            const auto* mBeta = payload->beta();
            const auto* mRho = payload->rho();
            const auto* mNu = payload->nu();
            validateMatrix2D(mAlpha, nExp, nTen, id);
            validateMatrix2D(mBeta, nExp, nTen, id);
            validateMatrix2D(mRho, nExp, nTen, id);
            validateMatrix2D(mNu, nExp, nTen, id);

            const int expected = nExp * nTen;
            std::vector<double> alpha;
            std::vector<double> beta;
            std::vector<double> rho;
            std::vector<double> nu;
            alpha.reserve(expected);
            beta.reserve(expected);
            rho.reserve(expected);
            nu.reserve(expected);
            for (int k = 0; k < expected; ++k) {
                double a = resolveMatrixValue(mAlpha, k, quotes, id);
                double bv = resolveMatrixValue(mBeta, k, quotes, id);
                double r = resolveMatrixValue(mRho, k, quotes, id);
                double n = resolveMatrixValue(mNu, k, quotes, id);
                if (!std::isfinite(a) || !std::isfinite(bv) || !std::isfinite(r) || !std::isfinite(n)) {
                    QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec alpha/beta/rho/nu must be finite for vol id: " + id);
                }
                if (!(a > 0.0)) {
                    QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec alpha must be > 0 for vol id: " + id);
                }
                if (bv < 0.0 || bv > 1.0) {
                    QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec beta must be in [0, 1] for vol id: " + id);
                }
                if (!(r > -1.0 && r < 1.0)) {
                    QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec rho must be in (-1, 1) for vol id: " + id);
                }
                if (!(n > 0.0)) {
                    QUANTRA_INVALID_ARGUMENT("SwaptionSabrParamsSpec nu must be > 0 for vol id: " + id);
                }
                alpha.push_back(a);
                beta.push_back(bv);
                rho.push_back(r);
                nu.push_back(n);
            }

            // Defer handle construction until finalizeSwaptionVolEntryForPricing
            // injects ATM forwards from the swap-index runtime. SABR per-node
            // smile sections require F(expiry, tenor), which is not available
            // at parse time.
            SwaptionVolEntry entry;
            entry.qlVolType = qlType;
            entry.displacement = disp;
            entry.referenceDate = ref;
            entry.calendar = cal;
            entry.businessDayConvention = bdc;
            entry.dayCounter = dc;
            entry.volKind = quantra::enums::SwaptionVolKind_SabrParams;
            entry.constantVol = std::numeric_limits<double>::quiet_NaN();
            entry.expiries = expiries;
            entry.tenors = tenors;
            entry.swapIndexId = wrapperSwapIndexId;
            entry.strikeKind = quantra::enums::SwaptionStrikeKind_Absolute;
            entry.allowExternalAtm = false;
            entry.sabrAlpha = std::move(alpha);
            entry.sabrBeta = std::move(beta);
            entry.sabrRho = std::move(rho);
            entry.sabrNu = std::move(nu);
            entry.nExp = nExp;
            entry.nTen = nTen;
            entry.nStrikes = 0;
            return entry;
        }

        case quantra::SwaptionVolPayload_SwaptionSabrCalibrateSpec: {
            auto* payload = wrapper->payload_as_SwaptionSabrCalibrateSpec();
            if (!payload || !payload->base()) {
                QUANTRA_INVALID_ARGUMENT("SwaptionSabrCalibrateSpec base missing for vol id: " + id);
            }
            const auto* b = payload->base();
            validateIrVolBaseCommon(b, id);

            // SABR via Hagan returns lognormal Black vol. Normal SABR is a
            // separate model with its own engine-pairing rules; reject for v1.
            if (requiredVolType(b, id) == quantra::enums::VolatilityType_Normal) {
                QUANTRA_INVALID_ARGUMENT(
                    "SwaptionSabrCalibrateSpec only supports Lognormal/ShiftedLognormal vol type "
                    "(Normal SABR is intentionally not supported for v1) for vol id: " + id);
            }

            // v1 rejects user-supplied per-strike weights. QuantLib 1.41
            // exposes only a cube-level vega-weighted flag (see
            // vega_weighted_smile_fit). Silently ignoring weight values would
            // be a misleading no-op; reject outright.
            if (payload->weights() && payload->weights()->values() &&
                payload->weights()->values()->size() > 0) {
                QUANTRA_INVALID_ARGUMENT(
                    "SwaptionSabrCalibrateSpec.weights: per-strike weights not supported in v1; "
                    "use vega_weighted_smile_fit instead, for vol id: " + id);
            }

            QuantLib::Date ref = DateToQL(b->reference_date()->str());
            QuantLib::Calendar cal = CalendarToQL(b->calendar().value());
            QuantLib::BusinessDayConvention bdc = ConventionToQL(b->business_day_convention().value());
            QuantLib::DayCounter dc = DayCounterToQL(b->day_counter().value());
            double disp = b->displacement();
            QuantLib::VolatilityType qlType = toQlVolType(requiredVolType(b, id));

            if (!payload->expiries() || !payload->tenors() || !payload->strikes()) {
                QUANTRA_INVALID_ARGUMENT("SwaptionSabrCalibrateSpec expiries/tenors/strikes missing for vol id: " + id);
            }
            int nExp = static_cast<int>(payload->expiries()->size());
            int nTen = static_cast<int>(payload->tenors()->size());
            int nStr = static_cast<int>(payload->strikes()->size());
            if (nExp <= 0 || nTen <= 0 || nStr <= 0) {
                QUANTRA_INVALID_ARGUMENT("SwaptionSabrCalibrateSpec expiries/tenors/strikes empty for vol id: " + id);
            }
            // QuantLib's XabrSwaptionVolatilityCube QL_REQUIREs at least 2
            // option times and 2 swap lengths to construct its internal
            // interpolators. Reject undersized grids at parse time with a
            // clear message rather than deferring to a deep QL_REQUIRE crash.
            if (nExp < 2 || nTen < 2) {
                QUANTRA_INVALID_ARGUMENT(
                    "SwaptionSabrCalibrateSpec requires at least 2 expiries and 2 tenors "
                    "for QuantLib cube interpolation, for vol id: " + id);
            }
            // SABR-cube needs at least 3 spread points per smile to fit 3 free
            // parameters (alpha, nu, rho) when beta is fixed, plus a 4th when
            // beta is free.
            const int minStrikes = (payload->beta_fixed() ? 3 : 4);
            if (nStr < minStrikes) {
                QUANTRA_INVALID_ARGUMENT(
                    "SwaptionSabrCalibrateSpec requires at least " +
                    std::to_string(minStrikes) + " strike spreads for the chosen beta_fixed setting "
                    "for vol id: " + id);
            }

            std::vector<QuantLib::Period> expiries;
            expiries.reserve(nExp);
            for (auto it = payload->expiries()->begin(); it != payload->expiries()->end(); ++it) {
                expiries.push_back(toQlPeriod(*it));
            }
            std::vector<QuantLib::Period> tenors;
            tenors.reserve(nTen);
            for (auto it = payload->tenors()->begin(); it != payload->tenors()->end(); ++it) {
                tenors.push_back(toQlPeriod(*it));
            }
            if (!std::is_sorted(expiries.begin(), expiries.end())) {
                QUANTRA_INVALID_ARGUMENT("SwaptionSabrCalibrateSpec expiries must be sorted ascending for vol id: " + id);
            }
            {
                auto dup = std::adjacent_find(
                    expiries.begin(), expiries.end(),
                    [](const QuantLib::Period& a, const QuantLib::Period& b) { return !(a < b); });
                if (dup != expiries.end()) {
                    QUANTRA_INVALID_ARGUMENT(
                        "SwaptionSabrCalibrateSpec expiries must be strictly increasing for vol id: " + id);
                }
            }
            if (!std::is_sorted(tenors.begin(), tenors.end())) {
                QUANTRA_INVALID_ARGUMENT("SwaptionSabrCalibrateSpec tenors must be sorted ascending for vol id: " + id);
            }
            {
                auto dup = std::adjacent_find(
                    tenors.begin(), tenors.end(),
                    [](const QuantLib::Period& a, const QuantLib::Period& b) { return !(a < b); });
                if (dup != tenors.end()) {
                    QUANTRA_INVALID_ARGUMENT(
                        "SwaptionSabrCalibrateSpec tenors must be strictly increasing for vol id: " + id);
                }
            }

            std::vector<double> strikeSpreads;
            strikeSpreads.reserve(nStr);
            for (auto it = payload->strikes()->begin(); it != payload->strikes()->end(); ++it) {
                strikeSpreads.push_back(*it);
            }
            if (!std::is_sorted(strikeSpreads.begin(), strikeSpreads.end())) {
                QUANTRA_INVALID_ARGUMENT(
                    "SwaptionSabrCalibrateSpec strikes (spreads from ATM) must be sorted ascending "
                    "for vol id: " + id);
            }
            {
                auto dup = std::adjacent_find(
                    strikeSpreads.begin(), strikeSpreads.end(),
                    [](double a, double b) { return !(a < b); });
                if (dup != strikeSpreads.end()) {
                    QUANTRA_INVALID_ARGUMENT(
                        "SwaptionSabrCalibrateSpec strikes must be strictly increasing for vol id: " + id);
                }
            }
            for (double s : strikeSpreads) {
                if (!std::isfinite(s)) {
                    QUANTRA_INVALID_ARGUMENT(
                        "SwaptionSabrCalibrateSpec strikes must be finite for vol id: " + id);
                }
            }

            const auto* t = payload->vols();
            validateTensor3D(t, nExp, nTen, nStr, id);
            const int expected = nExp * nTen * nStr;
            std::vector<double> marketVols;
            marketVols.reserve(expected);
            for (int k = 0; k < expected; ++k) {
                double v = resolveTensorValue(t, k, quotes, id);
                if (!std::isfinite(v) || v <= 0.0) {
                    QUANTRA_INVALID_ARGUMENT(
                        "SwaptionSabrCalibrateSpec vol must be > 0 and finite for vol id: " + id);
                }
                marketVols.push_back(v);
            }

            const double betaValue = payload->beta_value();
            if (payload->beta_fixed()) {
                if (!(betaValue >= 0.0 && betaValue <= 1.0)) {
                    QUANTRA_INVALID_ARGUMENT(
                        "SwaptionSabrCalibrateSpec beta_value must be in [0, 1] when beta_fixed=true "
                        "for vol id: " + id);
                }
            }

            // Defer handle construction until finalizeSwaptionVolEntryForPricing
            // injects ATM forwards. The QuantLib cube uses forwards internally
            // to convert strike spreads to absolute strikes during calibration,
            // and the swap_index_id + curve identities are not in scope at
            // parse time.
            SwaptionVolEntry entry;
            entry.qlVolType = qlType;
            entry.displacement = disp;
            entry.referenceDate = ref;
            entry.calendar = cal;
            entry.businessDayConvention = bdc;
            entry.dayCounter = dc;
            entry.volKind = quantra::enums::SwaptionVolKind_SabrCalibrate;
            entry.constantVol = std::numeric_limits<double>::quiet_NaN();
            entry.expiries = std::move(expiries);
            entry.tenors = std::move(tenors);
            entry.swapIndexId = wrapperSwapIndexId;
            // strikeKind = Absolute is the runtime contract: sampling uses
            // absolute strikes at query time. The spread vector is stored on
            // sabrStrikeSpreads where it cannot collide with the sampler's
            // bounds checks on entry.strikes (see vol_surface_parsers.h).
            entry.strikeKind = quantra::enums::SwaptionStrikeKind_Absolute;
            entry.allowExternalAtm = false;
            entry.sabrStrikeSpreads = std::move(strikeSpreads);
            entry.sabrMarketVolsFlat = std::move(marketVols);
            entry.sabrBetaFixed = payload->beta_fixed();
            entry.sabrBetaValue = betaValue;
            entry.sabrVegaWeightedSmileFit = payload->vega_weighted_smile_fit();
            entry.nExp = nExp;
            entry.nTen = nTen;
            entry.nStrikes = nStr;
            return entry;
        }

        default:
            QUANTRA_INVALID_ARGUMENT("Unknown SwaptionVolPayload type for vol id: " + id);
    }

    return SwaptionVolEntry();
}

// =============================================================================
// Black Vol Parser (for Equity/FX)
// =============================================================================

BlackVolEntry parseBlackVol(
    const quantra::VolSurfaceSpec* spec,
    const QuoteRegistry* quotes,
    const std::map<std::string, std::shared_ptr<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>>* curves) {
    if (!spec || !spec->id()) {
        QUANTRA_INVALID_ARGUMENT("VolSurfaceSpec or id is null");
    }
    std::string id = spec->id()->str();
    
    auto* payload = spec->payload_as_BlackVolSpec();
    if (!payload) {
        QUANTRA_INVALID_ARGUMENT("BlackVolSpec payload missing for vol id: " + id);
    }

    const auto* b = payload->base();
    validateBlackVolBase(b, id);

    QuantLib::Date ref = DateToQL(b->reference_date()->str());
    QuantLib::Calendar cal = CalendarToQL(b->calendar().value());
    QuantLib::BusinessDayConvention bdc = ConventionToQL(b->business_day_convention().value());
    QuantLib::DayCounter dc = DayCounterToQL(b->day_counter().value());
    const auto shape = b->shape();

    const bool hasExpiries = hasNonEmptyPeriods(payload->expiries());
    const bool hasStrikes = hasNonEmptyReals(payload->strikes());
    const bool hasTermVols = hasMatrixData(payload->term_vols());
    const bool hasSurfaceVols = hasMatrixData(payload->surface_vols());
    const bool hasPriceExpiries = hasNonEmptyStrings(payload->price_expiries());
    const bool hasPriceStrikes = hasNonEmptyReals(payload->price_strikes());
    const bool hasSurfacePrices = hasMatrixData(payload->surface_prices());
    const bool hasSpot = payload->spot() > 0.0 || hasNonEmptyString(payload->spot_quote_id());
    const bool hasDiscount =
        hasNonEmptyString(payload->discount_curve_id()) || payload->use_flat_discount_rate();
    const bool hasDividend =
        hasNonEmptyString(payload->dividend_curve_id()) || payload->use_flat_dividend_rate();

    switch (shape) {
        case quantra::enums::VolSurfaceShape_Constant:
            if (hasExpiries || hasStrikes || hasTermVols || hasSurfaceVols || hasPriceExpiries ||
                hasPriceStrikes || hasSurfacePrices || hasSpot || hasDiscount || hasDividend) {
                QUANTRA_INVALID_ARGUMENT(
                    "BlackVolSpec shape=Constant forbids grid/matrix/price-surface fields for vol id: " + id);
            }
            break;
        case quantra::enums::VolSurfaceShape_AtmMatrix2D:
            if (!hasExpiries || !hasTermVols) {
                QUANTRA_INVALID_ARGUMENT(
                    "BlackVolSpec shape=AtmMatrix2D requires expiries and term_vols for vol id: " + id);
            }
            if (hasStrikes || hasSurfaceVols || hasPriceExpiries || hasPriceStrikes || hasSurfacePrices || hasSpot ||
                hasDiscount || hasDividend) {
                QUANTRA_INVALID_ARGUMENT(
                    "BlackVolSpec shape=AtmMatrix2D forbids smile and price-surface fields for vol id: " + id);
            }
            break;
        case quantra::enums::VolSurfaceShape_SmileCube3D:
            if (!hasExpiries || !hasStrikes || !hasSurfaceVols) {
                QUANTRA_INVALID_ARGUMENT(
                    "BlackVolSpec shape=SmileCube3D requires expiries, strikes, and surface_vols for vol id: " + id);
            }
            if (hasTermVols || hasPriceExpiries || hasPriceStrikes || hasSurfacePrices || hasSpot || hasDiscount ||
                hasDividend) {
                QUANTRA_INVALID_ARGUMENT(
                    "BlackVolSpec shape=SmileCube3D forbids term and price-surface fields for vol id: " + id);
            }
            break;
        case quantra::enums::VolSurfaceShape_SurfaceFromPrices:
            if (!hasPriceExpiries || !hasPriceStrikes || !hasSurfacePrices || !hasSpot || !hasDiscount ||
                !hasDividend) {
                QUANTRA_INVALID_ARGUMENT(
                    "BlackVolSpec shape=SurfaceFromPrices requires price_expiries, price_strikes, surface_prices, "
                    "spot(spot or spot_quote_id), and discount/dividend inputs for vol id: " + id);
            }
            if (hasTermVols || hasSurfaceVols || hasExpiries || hasStrikes) {
                QUANTRA_INVALID_ARGUMENT(
                    "BlackVolSpec shape=SurfaceFromPrices forbids term_vols/surface_vols/expiries/strikes "
                    "for vol id: " + id);
            }
            break;
        default:
            QUANTRA_INVALID_ARGUMENT("Unsupported BlackVolSpec shape for vol id: " + id);
    }

    auto build_equity_black_vol_surface =
        [&]() -> std::pair<QuantLib::Handle<QuantLib::BlackVolTermStructure>, double> {
        switch (shape) {
            case quantra::enums::VolSurfaceShape_Constant: {
                double vol = resolveVolValue(b->constant_vol(), b->quote_id(), quotes, id);
                auto qlVol = std::make_shared<QuantLib::BlackConstantVol>(ref, cal, vol, dc);
                return {QuantLib::Handle<QuantLib::BlackVolTermStructure>(qlVol), vol};
            }

            case quantra::enums::VolSurfaceShape_AtmMatrix2D: {
                validateSupportedInterpolator(payload->expiry_interpolator(), "expiry_interpolator", id);
                if (!payload->expiries() || payload->expiries()->size() == 0) {
                    QUANTRA_INVALID_ARGUMENT("BlackVolSpec.expiries is required for shape=AtmMatrix2D, vol id: " + id);
                }
                const int nExp = static_cast<int>(payload->expiries()->size());
                const auto* termVols = payload->term_vols();
                validateMatrix2D(termVols, nExp, 1, id);

                std::vector<QuantLib::Date> dates;
                dates.reserve(nExp);
                std::vector<QuantLib::Volatility> vols;
                vols.reserve(nExp);

                for (int i = 0; i < nExp; ++i) {
                    QuantLib::Period p = toQlPeriod(payload->expiries()->Get(i));
                    QuantLib::Date d = cal.advance(ref, p, bdc);
                    if (d <= ref) {
                        QUANTRA_INVALID_ARGUMENT("BlackVolSpec.expiries must be after reference_date for vol id: " + id);
                    }
                    if (!dates.empty() && d <= dates.back()) {
                        QUANTRA_INVALID_ARGUMENT("BlackVolSpec.expiries must be strictly increasing for vol id: " + id);
                    }
                    double v = resolveMatrixValue(termVols, i, quotes, id);
                    if (v <= 0.0) {
                        QUANTRA_INVALID_ARGUMENT("BlackVolSpec term vol must be > 0 for vol id: " + id);
                    }
                    dates.push_back(d);
                    vols.push_back(v);
                }

                auto qlVol = std::make_shared<QuantLib::BlackVarianceCurve>(ref, dates, vols, dc, true);
                return {
                    QuantLib::Handle<QuantLib::BlackVolTermStructure>(qlVol),
                    std::numeric_limits<double>::quiet_NaN()};
            }

            case quantra::enums::VolSurfaceShape_SmileCube3D: {
                const auto surfaceInterp = resolveBlackSurfaceInterpolation(payload, id);
                if (!payload->expiries() || payload->expiries()->size() == 0) {
                    QUANTRA_INVALID_ARGUMENT("BlackVolSpec.expiries is required for shape=SmileCube3D, vol id: " + id);
                }
                if (!payload->strikes() || payload->strikes()->size() == 0) {
                    QUANTRA_INVALID_ARGUMENT("BlackVolSpec.strikes is required for shape=SmileCube3D, vol id: " + id);
                }
                const int nExp = static_cast<int>(payload->expiries()->size());
                const int nStr = static_cast<int>(payload->strikes()->size());
                const auto* surfaceVols = payload->surface_vols();
                validateMatrix2D(surfaceVols, nExp, nStr, id);

                std::vector<QuantLib::Date> dates;
                dates.reserve(nExp);
                for (int i = 0; i < nExp; ++i) {
                    QuantLib::Period p = toQlPeriod(payload->expiries()->Get(i));
                    QuantLib::Date d = cal.advance(ref, p, bdc);
                    if (d <= ref) {
                        QUANTRA_INVALID_ARGUMENT("BlackVolSpec.expiries must be after reference_date for vol id: " + id);
                    }
                    if (!dates.empty() && d <= dates.back()) {
                        QUANTRA_INVALID_ARGUMENT("BlackVolSpec.expiries must be strictly increasing for vol id: " + id);
                    }
                    dates.push_back(d);
                }

                std::vector<QuantLib::Real> strikes;
                strikes.reserve(nStr);
                for (int j = 0; j < nStr; ++j) {
                    strikes.push_back(payload->strikes()->Get(j));
                    if (j > 0 && !(strikes[j] > strikes[j - 1])) {
                        QUANTRA_INVALID_ARGUMENT("BlackVolSpec.strikes must be strictly increasing for vol id: " + id);
                    }
                }

                QuantLib::Matrix blackVolMatrix(nStr, nExp);
                for (int i = 0; i < nExp; ++i) {
                    for (int j = 0; j < nStr; ++j) {
                        const int idx = i * nStr + j;
                        const double v = resolveMatrixValue(surfaceVols, idx, quotes, id);
                        if (v <= 0.0) {
                            QUANTRA_INVALID_ARGUMENT("BlackVolSpec surface vol must be > 0 for vol id: " + id);
                        }
                        blackVolMatrix[j][i] = v;
                    }
                }

                auto qlVol =
                    std::make_shared<QuantLib::BlackVarianceSurface>(ref, cal, dates, strikes, blackVolMatrix, dc);
                if (surfaceInterp == SurfaceInterpolationMode::Bicubic) {
                    qlVol->setInterpolation<QuantLib::Bicubic>();
                } else {
                    qlVol->setInterpolation<QuantLib::Bilinear>();
                }
                return {
                    QuantLib::Handle<QuantLib::BlackVolTermStructure>(qlVol),
                    std::numeric_limits<double>::quiet_NaN()};
            }

            case quantra::enums::VolSurfaceShape_SurfaceFromPrices: {
                const auto surfaceInterp = resolveBlackSurfaceInterpolation(payload, id);

                if (!payload->price_expiries() || payload->price_expiries()->size() == 0) {
                    QUANTRA_INVALID_ARGUMENT("BlackVolSpec.price_expiries is required for shape=SurfaceFromPrices, vol id: " + id);
                }
                if (!payload->price_strikes() || payload->price_strikes()->size() == 0) {
                    QUANTRA_INVALID_ARGUMENT("BlackVolSpec.price_strikes is required for shape=SurfaceFromPrices, vol id: " + id);
                }
                const int nExp = static_cast<int>(payload->price_expiries()->size());
                const int nStr = static_cast<int>(payload->price_strikes()->size());
                const auto* surfacePrices = payload->surface_prices();
                validateMatrix2D(surfacePrices, nExp, nStr, id);

                std::vector<QuantLib::Date> dates;
                dates.reserve(nExp);
                for (int i = 0; i < nExp; ++i) {
                    auto* expiry = payload->price_expiries()->Get(i);
                    if (!expiry || expiry->size() == 0) {
                        QUANTRA_INVALID_ARGUMENT("BlackVolSpec.price_expiries entries must be non-empty for vol id: " + id);
                    }
                    QuantLib::Date d = DateToQL(expiry->str());
                    if (d <= ref) {
                        QUANTRA_INVALID_ARGUMENT("BlackVolSpec.price_expiries must be after reference_date for vol id: " + id);
                    }
                    if (!dates.empty() && d <= dates.back()) {
                        QUANTRA_INVALID_ARGUMENT("BlackVolSpec.price_expiries must be strictly increasing for vol id: " + id);
                    }
                    dates.push_back(d);
                }

                std::vector<QuantLib::Real> strikes;
                strikes.reserve(nStr);
                for (int j = 0; j < nStr; ++j) {
                    strikes.push_back(payload->price_strikes()->Get(j));
                    if (j > 0 && !(strikes[j] > strikes[j - 1])) {
                        QUANTRA_INVALID_ARGUMENT("BlackVolSpec.price_strikes must be strictly increasing for vol id: " + id);
                    }
                }

                QuantLib::Handle<QuantLib::Quote> spot;
                if (payload->spot_quote_id() && !payload->spot_quote_id()->str().empty()) {
                    if (!quotes) {
                        QUANTRA_ERROR("spot_quote_id requires QuoteRegistry for vol id: " + id);
                    }
                    spot = quotes->getHandle(payload->spot_quote_id()->str());
                } else {
                    const double spotLevel = payload->spot();
                    if (!(spotLevel > 0.0)) {
                        QUANTRA_INVALID_ARGUMENT("BlackVolSpec.spot must be > 0 when spot_quote_id is not provided for vol id: " + id);
                    }
                    spot = QuantLib::Handle<QuantLib::Quote>(
                        std::make_shared<QuantLib::SimpleQuote>(spotLevel));
                }

                std::shared_ptr<QuantLib::YieldTermStructure> flatDiscount;
                std::shared_ptr<QuantLib::YieldTermStructure> flatDividend;
                QuantLib::Handle<QuantLib::YieldTermStructure> discount;
                QuantLib::Handle<QuantLib::YieldTermStructure> dividend;

                if (payload->discount_curve_id() && !payload->discount_curve_id()->str().empty()) {
                    if (!curves) {
                        QUANTRA_ERROR(
                            "discount_curve_id requires PricingRegistry curves for vol id: " + id);
                    }
                    auto it = curves->find(payload->discount_curve_id()->str());
                    if (it == curves->end()) {
                        QUANTRA_NOT_FOUND("Discount curve not found for vol id: " + id);
                    }
                    discount = QuantLib::Handle<QuantLib::YieldTermStructure>(it->second->currentLink());
                } else if (payload->use_flat_discount_rate()) {
                    flatDiscount = std::make_shared<QuantLib::FlatForward>(ref, payload->flat_discount_rate(), dc);
                    discount = QuantLib::Handle<QuantLib::YieldTermStructure>(flatDiscount);
                } else {
                    QUANTRA_INVALID_ARGUMENT(
                        "SurfaceFromPrices requires discount_curve_id or use_flat_discount_rate=true for vol id: " + id);
                }

                if (payload->dividend_curve_id() && !payload->dividend_curve_id()->str().empty()) {
                    if (!curves) {
                        QUANTRA_ERROR(
                            "dividend_curve_id requires PricingRegistry curves for vol id: " + id);
                    }
                    auto it = curves->find(payload->dividend_curve_id()->str());
                    if (it == curves->end()) {
                        QUANTRA_NOT_FOUND("Dividend curve not found for vol id: " + id);
                    }
                    dividend = QuantLib::Handle<QuantLib::YieldTermStructure>(it->second->currentLink());
                } else if (payload->use_flat_dividend_rate()) {
                    flatDividend = std::make_shared<QuantLib::FlatForward>(ref, payload->flat_dividend_rate(), dc);
                    dividend = QuantLib::Handle<QuantLib::YieldTermStructure>(flatDividend);
                } else {
                    QUANTRA_INVALID_ARGUMENT(
                        "SurfaceFromPrices requires dividend_curve_id or use_flat_dividend_rate=true for vol id: " + id);
                }

                auto volQuote = QuantLib::Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(0.20));
                auto volTs = std::make_shared<QuantLib::BlackConstantVol>(ref, cal, volQuote, dc);
                auto process = std::make_shared<QuantLib::BlackScholesMertonProcess>(
                    spot,
                    dividend,
                    discount,
                    QuantLib::Handle<QuantLib::BlackVolTermStructure>(volTs));
                const auto optionType = toQlEquityOptionType(payload->price_option_type(), id);

                QuantLib::Matrix blackVolMatrix(nStr, nExp);
                for (int i = 0; i < nExp; ++i) {
                    const QuantLib::Date expiry = dates[i];
                    const double t = dc.yearFraction(ref, expiry);
                    if (t <= 0.0) {
                        QUANTRA_ERROR("Computed non-positive option time for SurfaceFromPrices, vol id: " + id);
                    }
                    auto exercise = std::make_shared<QuantLib::EuropeanExercise>(expiry);
                    for (int j = 0; j < nStr; ++j) {
                        const int idx = i * nStr + j;
                        const double strike = strikes[j];
                        const double price =
                            resolveMatrixValueAnyType(surfacePrices, idx, quotes, id, "surface_prices");
                        if (!(price > 0.0)) {
                            QUANTRA_INVALID_ARGUMENT("BlackVolSpec surface price must be > 0 for vol id: " + id);
                        }
                        const double dfRiskFree = discount->discount(expiry);
                        const double dfDividend = dividend->discount(expiry);
                        const double forwardDiscountedSpot = spot->value() * dfDividend;
                        const double discountedStrike = strike * dfRiskFree;
                        double lowerBound = 0.0;
                        double upperBound = 0.0;
                        if (optionType == QuantLib::Option::Call) {
                            lowerBound = std::max(0.0, forwardDiscountedSpot - discountedStrike);
                            upperBound = forwardDiscountedSpot;
                        } else {
                            lowerBound = std::max(0.0, discountedStrike - forwardDiscountedSpot);
                            upperBound = discountedStrike;
                        }
                        const double tolerance = 1.0e-10 * std::max(1.0, upperBound);
                        if (price < lowerBound - tolerance || price > upperBound + tolerance) {
                            std::ostringstream msg;
                            msg << "SurfaceFromPrices price violates Black-Scholes bounds at expiry="
                                << DateToIso(expiry)
                                << ", strike=" << strike
                                << ", price=" << price
                                << ", lower=" << lowerBound
                                << ", upper=" << upperBound
                                << " for vol id: " << id;
                            QUANTRA_INVALID_ARGUMENT(
                                msg.str());
                        }

                        auto payoff =
                            std::make_shared<QuantLib::PlainVanillaPayoff>(optionType, strike);
                        QuantLib::VanillaOption opt(payoff, exercise);
                        try {
                            const double implied = opt.impliedVolatility(
                                price,
                                process,
                                1.0e-8,
                                500,
                                1.0e-8,
                                10.0);
                            if (!(implied > 0.0) || !std::isfinite(implied)) {
                                QUANTRA_INVALID_ARGUMENT("Non-positive implied vol recovered from price grid");
                            }
                            blackVolMatrix[j][i] = implied;
                        } catch (const std::exception& e) {
                            QUANTRA_INVALID_ARGUMENT(
                                "Failed implied-vol inversion at expiry index " + std::to_string(i) +
                                ", strike index " + std::to_string(j) +
                                " for vol id: " + id + ": " + e.what());
                        }
                    }
                }

                auto qlVol =
                    std::make_shared<QuantLib::BlackVarianceSurface>(ref, cal, dates, strikes, blackVolMatrix, dc);
                if (surfaceInterp == SurfaceInterpolationMode::Bicubic) {
                    qlVol->setInterpolation<QuantLib::Bicubic>();
                } else {
                    qlVol->setInterpolation<QuantLib::Bilinear>();
                }
                return {
                    QuantLib::Handle<QuantLib::BlackVolTermStructure>(qlVol),
                    std::numeric_limits<double>::quiet_NaN()};
            }

            default:
                QUANTRA_INVALID_ARGUMENT("Unsupported BlackVolSpec shape for vol id: " + id);
        }
        return {};
    };

    auto built = build_equity_black_vol_surface();
    BlackVolEntry entry;
    entry.handle = built.first;
    entry.constantVol = built.second;
    entry.referenceDate = ref;
    entry.calendar = cal;
    entry.calendarFb = b->calendar().value();
    entry.businessDayConvention = bdc;
    entry.businessDayConventionFb = b->business_day_convention().value();
    entry.dayCounter = dc;
    if (payload->allow_extrapolation()) {
        entry.handle->enableExtrapolation();
    } else {
        entry.handle->disableExtrapolation();
    }
    return entry;
}

SwaptionVolEntry bumpSwaptionVolEntry(const SwaptionVolEntry& base, double volBump) {
    if (volBump == 0.0) {
        return base;
    }

    SwaptionVolEntry entry = base;

    switch (base.volKind) {
        case quantra::enums::SwaptionVolKind_Constant: {
            double bumpedVol = base.constantVol + volBump;
            if (bumpedVol <= 0.0) bumpedVol = 1.0e-8;
            auto qlVol = std::make_shared<QuantLib::ConstantSwaptionVolatility>(
                base.referenceDate,
                base.calendar,
                base.businessDayConvention,
                bumpedVol,
                base.dayCounter,
                base.qlVolType,
                base.displacement);
            entry.handle = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(qlVol);
            entry.constantVol = bumpedVol;
            return entry;
        }

        case quantra::enums::SwaptionVolKind_AtmMatrix2D: {
            int nExp = base.nExp;
            int nTen = base.nTen;
            if (nExp <= 0 || nTen <= 0 || static_cast<int>(base.volsFlat.size()) != nExp * nTen) {
                QUANTRA_ERROR("SwaptionVolEntry ATM matrix dims invalid for bump");
            }
            QuantLib::Matrix vols(nExp, nTen);
            std::vector<double> flat;
            flat.reserve(nExp * nTen);
            for (int i = 0; i < nExp; i++) {
                for (int j = 0; j < nTen; j++) {
                    int idx = i * nTen + j;
                    double v = base.volsFlat[idx] + volBump;
                    if (v <= 0.0) v = 1.0e-8;
                    vols[i][j] = v;
                    flat.push_back(v);
                }
            }
            QuantLib::Matrix shifts;
            if (base.displacement != 0.0) {
                shifts = QuantLib::Matrix(nExp, nTen, base.displacement);
            }
            auto qlVol = std::make_shared<QuantLib::SwaptionVolatilityMatrix>(
                base.referenceDate,
                base.calendar,
                base.businessDayConvention,
                base.expiries,
                base.tenors,
                vols,
                base.dayCounter,
                false,
                base.qlVolType,
                shifts);
            entry.handle = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(qlVol);
            entry.volsFlat = flat;
            return entry;
        }

        case quantra::enums::SwaptionVolKind_SmileCube3D: {
            int nExp = base.nExp;
            int nTen = base.nTen;
            int nStr = base.nStrikes;
            if (nExp <= 0 || nTen <= 0 || nStr <= 0 ||
                static_cast<int>(base.volsFlat.size()) != nExp * nTen * nStr) {
                QUANTRA_ERROR("SwaptionVolEntry smile cube dims invalid for bump");
            }
            std::vector<double> vols = base.volsFlat;
            for (auto& v : vols) {
                v += volBump;
                if (v <= 0.0) v = 1.0e-8;
            }
            entry.volsFlat = vols;
            const bool needsAtm =
                (base.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM);
            const bool hasAtm = !base.atmForwardsFlat.empty();
            if (!(needsAtm && !hasAtm)) {
                auto qlVol = std::make_shared<SwaptionSmileCubeCustom>(
                    base.referenceDate,
                    base.calendar,
                    base.businessDayConvention,
                    base.dayCounter,
                    base.qlVolType,
                    base.displacement,
                    base.expiries,
                    base.tenors,
                    base.strikes,
                    base.strikeKind,
                    base.atmForwardsFlat,
                    vols);
                entry.handle = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(qlVol);
            } else {
                // Keep handle deferred; finalizeSwaptionVolEntryForPricing injects ATM and builds it.
                entry.handle = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>();
            }
            return entry;
        }

        case quantra::enums::SwaptionVolKind_SabrParams: {
            // Placeholder semantics: for now we treat SABR risk as unsupported until
            // forward-aware SABR cube wiring is implemented.
            QUANTRA_NOT_IMPLEMENTED("SABR bump semantics are placeholder-only; runtime bumping is not supported yet");
        }

        case quantra::enums::SwaptionVolKind_SabrCalibrate:
        default:
            QUANTRA_NOT_IMPLEMENTED("Vol bump not supported for this swaption vol kind");
    }

    return SwaptionVolEntry();
}

SwaptionVolEntry withSwaptionSmileCubeAtm(
    const SwaptionVolEntry& base,
    const std::vector<double>& atmForwardsFlat) {
    if (base.volKind != quantra::enums::SwaptionVolKind_SmileCube3D) {
        return base;
    }
    if (base.nExp <= 0 || base.nTen <= 0 || base.nStrikes <= 0) {
        QUANTRA_ERROR("Invalid smile cube dimensions while injecting ATM forwards");
    }
    if (static_cast<int>(atmForwardsFlat.size()) != base.nExp * base.nTen) {
        QUANTRA_ERROR("ATM forward matrix size mismatch while injecting ATM forwards");
    }
    if (base.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM &&
        base.swapIndexId.empty()) {
        QUANTRA_ERROR("Injecting ATM into SpreadFromATM cube requires swapIndexId to be set");
    }

    SwaptionVolEntry out = base;
    auto qlVol = std::make_shared<SwaptionSmileCubeCustom>(
        base.referenceDate,
        base.calendar,
        base.businessDayConvention,
        base.dayCounter,
        base.qlVolType,
        base.displacement,
        base.expiries,
        base.tenors,
        base.strikes,
        base.strikeKind,
        atmForwardsFlat,
        base.volsFlat);

    out.handle = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(qlVol);
    out.atmForwardsFlat = atmForwardsFlat;
    return out;
}

namespace {

// Linearly interpolate per-node ATM vol at spread=0 from the market vol grid.
// Strike spreads are sorted ascending; bracket the zero spread, and linearly
// interpolate the two adjacent vols. Flat-extends if all spreads are positive
// or all negative.
double interpolateAtmVolAtSpreadZero(
    const std::vector<double>& strikeSpreads,
    const double* nodeVols,
    int nStrikes) {
    if (nStrikes <= 0) {
        QUANTRA_ERROR("SwaptionSabrCalibrateSpec internal error: empty strike grid");
    }
    if (nStrikes == 1) return nodeVols[0];
    if (strikeSpreads.front() >= 0.0) return nodeVols[0];
    if (strikeSpreads.back() <= 0.0) return nodeVols[nStrikes - 1];
    // Find lo, hi such that strikeSpreads[lo] < 0 <= strikeSpreads[hi].
    auto it = std::lower_bound(strikeSpreads.begin(), strikeSpreads.end(), 0.0);
    int hi = static_cast<int>(it - strikeSpreads.begin());
    int lo = hi - 1;
    double x0 = strikeSpreads[lo], x1 = strikeSpreads[hi];
    double w = (0.0 - x0) / (x1 - x0);
    return nodeVols[lo] * (1.0 - w) + nodeVols[hi] * w;
}

} // namespace

SwaptionVolEntry withSwaptionSabrCalibrateAtm(
    const SwaptionVolEntry& base,
    const std::vector<double>& atmForwardsFlat,
    const std::shared_ptr<QuantLib::SwapIndex>& swapIndexBase,
    const std::string& cubeCacheKey) {
    if (base.volKind != quantra::enums::SwaptionVolKind_SabrCalibrate) {
        return base;
    }
    if (base.nExp <= 0 || base.nTen <= 0 || base.nStrikes <= 0) {
        QUANTRA_ERROR("Invalid SABR calibrate dimensions while building handle");
    }
    const int nExp = base.nExp;
    const int nTen = base.nTen;
    const int nStr = base.nStrikes;
    const int expected2d = nExp * nTen;
    const int expected3d = nExp * nTen * nStr;
    if (static_cast<int>(atmForwardsFlat.size()) != expected2d) {
        QUANTRA_ERROR("ATM forward matrix size mismatch for SABR calibrate cube");
    }
    if (static_cast<int>(base.sabrMarketVolsFlat.size()) != expected3d) {
        QUANTRA_ERROR("SABR market vol tensor size mismatch in calibrate finalize");
    }
    if (static_cast<int>(base.sabrStrikeSpreads.size()) != nStr) {
        QUANTRA_ERROR("SABR strike spread vector size mismatch in calibrate finalize");
    }
    if (!swapIndexBase) {
        QUANTRA_ERROR("SABR calibrate finalize requires a non-null swap_index_base");
    }
    if (base.swapIndexId.empty()) {
        QUANTRA_ERROR("Building SABR calibrate handle requires swapIndexId to be set");
    }

    // Cache lookup. Empty cache key is a sentinel for "do not cache" (used by
    // sanity hooks); we always build a fresh cube in that case. The cache is
    // also gated by QUANTRA_SABR_CACHE_ENABLED (default off) — when disabled we
    // skip tryGet/put entirely and recalibrate fresh each request. Disabling the
    // cache does NOT revert to the live cube: we still freeze the calibrated
    // params into a SwaptionSabrParamsCube below; we just don't store/reuse it.
    auto& cache = SabrCalibrateCache::instance();
    const bool cacheEnabled = SabrCalibrateCache::enabled() && !cubeCacheKey.empty();
    std::shared_ptr<const SabrCalibratedCube> cached;
    if (cacheEnabled) {
        cached = cache.tryGet(cubeCacheKey);
    }

    auto produceEntry = [&](std::shared_ptr<const SabrCalibratedCube> cube) -> SwaptionVolEntry {
        SwaptionVolEntry out = base;
        out.handle = cube->handle;
        out.atmForwardsFlat = atmForwardsFlat;
        out.sabrAlpha = cube->alpha;
        out.sabrBeta = cube->beta;
        out.sabrRho = cube->rho;
        out.sabrNu = cube->nu;
        out.sabrPerNodeRmse = cube->perNodeRmse;
        out.sabrPerNodeMaxError = cube->perNodeMaxError;
        out.sabrPerNodeEndCriteria = cube->perNodeEndCriteria;
        return out;
    };

    if (cached) {
        return produceEntry(cached);
    }

    // -----------------------------------------------------------------------
    // Build the per-node ATM vol structure that the QL cube takes as its
    // atmVolStructure handle.
    //
    // The cube treats the user-supplied vols as ATM + spread, so we must give
    // it (a) an ATM matrix evaluated at each (expiry, tenor) node and (b) the
    // per-(strike, expiry, tenor) vol spreads (vols - ATM). The ATM vol at
    // each node is recovered by linearly interpolating the user's strike grid
    // at spread = 0 — this works regardless of whether 0.0 is in the grid.
    // -----------------------------------------------------------------------
    std::vector<std::vector<double>> atmVols2d(nExp, std::vector<double>(nTen, 0.0));
    for (int i = 0; i < nExp; ++i) {
        for (int j = 0; j < nTen; ++j) {
            const double* nodeVols = &base.sabrMarketVolsFlat[(i * nTen + j) * nStr];
            atmVols2d[i][j] = interpolateAtmVolAtSpreadZero(base.sabrStrikeSpreads, nodeVols, nStr);
            if (!(atmVols2d[i][j] > 0.0)) {
                QUANTRA_ERROR(
                    "SABR calibrate: interpolated ATM vol at (expiryIdx=" + std::to_string(i) +
                    ", tenorIdx=" + std::to_string(j) + ") is non-positive");
            }
        }
    }
    QuantLib::Matrix atmVolMatrix(nExp, nTen, 0.0);
    for (int i = 0; i < nExp; ++i) {
        for (int j = 0; j < nTen; ++j) {
            atmVolMatrix[i][j] = atmVols2d[i][j];
        }
    }
    QuantLib::Matrix shifts;
    if (base.displacement != 0.0) {
        shifts = QuantLib::Matrix(nExp, nTen, base.displacement);
    }
    auto atmMatrix = std::make_shared<QuantLib::SwaptionVolatilityMatrix>(
        base.referenceDate, base.calendar, base.businessDayConvention,
        base.expiries, base.tenors, atmVolMatrix, base.dayCounter, false, base.qlVolType, shifts);
    QuantLib::Handle<QuantLib::SwaptionVolatilityStructure> atmVolHandle(atmMatrix);

    // Vol spreads as Handle<Quote>. Outer dim is options*tenors row-major
    // (j*nSwapTenors + k), inner dim is strikes — matches the QL cube convention.
    std::vector<QuantLib::Spread> strikeSpreadsQl(
        base.sabrStrikeSpreads.begin(), base.sabrStrikeSpreads.end());
    std::vector<std::vector<QuantLib::Handle<QuantLib::Quote>>> volSpreads;
    volSpreads.reserve(static_cast<size_t>(expected2d));
    for (int i = 0; i < nExp; ++i) {
        for (int j = 0; j < nTen; ++j) {
            std::vector<QuantLib::Handle<QuantLib::Quote>> row;
            row.reserve(nStr);
            const double atm = atmVols2d[i][j];
            for (int k = 0; k < nStr; ++k) {
                const double v = base.sabrMarketVolsFlat[(i * nTen + j) * nStr + k];
                row.push_back(QuantLib::Handle<QuantLib::Quote>(
                    std::make_shared<QuantLib::SimpleQuote>(v - atm)));
            }
            volSpreads.push_back(std::move(row));
        }
    }

    // Parameters guess: 4 params per node in QL internal order {alpha, beta, nu, rho}.
    // For each parameter we provide a starting guess and a per-cube fixed flag.
    // We let the SABR optimizer pick sensible defaults for alpha/nu/rho via
    // its built-in heuristics (passing the typical 0.04 / 0.4 / 0.0 levels).
    std::vector<std::vector<QuantLib::Handle<QuantLib::Quote>>> parametersGuess;
    parametersGuess.reserve(static_cast<size_t>(expected2d));
    for (int i = 0; i < nExp; ++i) {
        for (int j = 0; j < nTen; ++j) {
            std::vector<QuantLib::Handle<QuantLib::Quote>> row;
            row.reserve(4);
            // alpha
            row.push_back(QuantLib::Handle<QuantLib::Quote>(
                std::make_shared<QuantLib::SimpleQuote>(0.04)));
            // beta
            row.push_back(QuantLib::Handle<QuantLib::Quote>(
                std::make_shared<QuantLib::SimpleQuote>(base.sabrBetaValue)));
            // nu
            row.push_back(QuantLib::Handle<QuantLib::Quote>(
                std::make_shared<QuantLib::SimpleQuote>(0.4)));
            // rho
            row.push_back(QuantLib::Handle<QuantLib::Quote>(
                std::make_shared<QuantLib::SimpleQuote>(0.0)));
            parametersGuess.push_back(std::move(row));
        }
    }
    // Fixed flags follow the same internal {alpha, beta, nu, rho} order.
    std::vector<bool> isParameterFixed = {false, base.sabrBetaFixed, false, false};

    // Build and force calibration so sparseSabrParameters() is populated.
    auto qlCube =
        std::make_shared<QuantLib::SabrSwaptionVolatilityCube>(
            atmVolHandle,
            base.expiries,
            base.tenors,
            strikeSpreadsQl,
            volSpreads,
            swapIndexBase,
            swapIndexBase,        // shortSwapIndexBase = swapIndexBase for v1 (single SwapIndex)
            base.sabrVegaWeightedSmileFit,
            parametersGuess,
            isParameterFixed,
            /*isAtmCalibrated=*/false);
    qlCube->enableExtrapolation();

    // Triggers per-node calibration. Throws on convergence/tolerance failure.
    QuantLib::Matrix browsed = qlCube->sparseSabrParameters();
    // browse() shape: rows = swapLengths.size() * optionTimes.size(), one row per
    // (swapTenorIdx, optionIdx); columns = [swapLength, optionTime, alpha, beta,
    // nu, rho, forward, rmsError, maxError, endCriteria]. We re-index into our
    // row-major (i_expiry, j_tenor) layout.
    if (static_cast<int>(browsed.rows()) != expected2d || browsed.columns() < 10) {
        QUANTRA_ERROR(
            "SABR calibrate: unexpected sparseSabrParameters shape " +
            std::to_string(browsed.rows()) + "x" + std::to_string(browsed.columns()));
    }
    auto out = std::make_shared<SabrCalibratedCube>();
    out->alpha.assign(expected2d, 0.0);
    out->beta.assign(expected2d, 0.0);
    out->rho.assign(expected2d, 0.0);
    out->nu.assign(expected2d, 0.0);
    out->perNodeRmse.assign(expected2d, 0.0);
    out->perNodeMaxError.assign(expected2d, 0.0);
    out->perNodeEndCriteria.assign(expected2d, static_cast<int>(QuantLib::EndCriteria::None));
    out->calibratedForwards.assign(expected2d, 0.0);
    for (int i = 0; i < nExp; ++i) {
        for (int j = 0; j < nTen; ++j) {
            // browse() row layout is (swapLengthIdx * optionTimes.size() + optionIdx),
            // per Cube::browse() in QL source: result[i*optionTimes_.size()+j][...].
            const std::size_t row =
                static_cast<std::size_t>(j) * static_cast<std::size_t>(nExp) +
                static_cast<std::size_t>(i);
            const int k = i * nTen + j;
            out->alpha[k] = browsed[row][2];
            out->beta[k]  = browsed[row][3];
            // QL browse layer order (per XabrSwaptionVolatilityCube::Cube
            // setLayer calls): 0=alpha, 1=beta, 2=nu, 3=rho, 4=forward,
            // 5=rmsError, 6=maxError, 7=endCriteria. browse() emits these as
            // columns 2..9. We translate QL's internal {alpha,beta,nu,rho} to
            // our schema field order {alpha,beta,rho,nu} here.
            out->nu[k]    = browsed[row][4];
            out->rho[k]   = browsed[row][5];
            out->calibratedForwards[k] = browsed[row][6];
            out->perNodeRmse[k] = browsed[row][7];
            out->perNodeMaxError[k] = browsed[row][8];
            out->perNodeEndCriteria[k] = static_cast<int>(browsed[row][9]);
        }
    }

    // qlCube was needed only to run the per-node Levenberg-Marquardt fit and to
    // extract the calibrated params + forwards above; it is now discarded. We do
    // NOT cache or return the live cube: it is a LazyObject that observes its
    // swap index -> underlying curve handle and re-runs the full calibration
    // whenever that handle is relinked (per-request churn). Instead we freeze the
    // extracted params + per-node forwards into a SwaptionSabrParamsCube, which
    // observes nothing and reproduces the calibrated smiles at each node. This is
    // built identically to the params-provided path in withSwaptionSabrParamsAtm.
    auto frozenCube = std::make_shared<SwaptionSabrParamsCube>(
        base.referenceDate,
        base.calendar,
        base.businessDayConvention,
        base.dayCounter,
        base.qlVolType,
        base.displacement,
        base.expiries,
        base.tenors,
        out->alpha,
        out->beta,
        out->rho,
        out->nu,
        out->calibratedForwards);
    out->handle = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(frozenCube);

    if (cacheEnabled) {
        cache.put(cubeCacheKey, out);
    }
    return produceEntry(out);
}

SwaptionVolEntry withSwaptionSabrParamsAtm(
    const SwaptionVolEntry& base,
    const std::vector<double>& atmForwardsFlat) {
    if (base.volKind != quantra::enums::SwaptionVolKind_SabrParams) {
        return base;
    }
    if (base.nExp <= 0 || base.nTen <= 0) {
        QUANTRA_ERROR("Invalid SABR params dimensions while injecting ATM forwards");
    }
    const int expected = base.nExp * base.nTen;
    if (static_cast<int>(atmForwardsFlat.size()) != expected) {
        QUANTRA_ERROR("ATM forward matrix size mismatch while injecting ATM forwards into SABR cube");
    }
    if (static_cast<int>(base.sabrAlpha.size()) != expected ||
        static_cast<int>(base.sabrBeta.size()) != expected ||
        static_cast<int>(base.sabrRho.size()) != expected ||
        static_cast<int>(base.sabrNu.size()) != expected) {
        QUANTRA_ERROR("SABR parameter grid sizes inconsistent while injecting ATM forwards");
    }
    if (base.swapIndexId.empty()) {
        QUANTRA_ERROR("Building SABR params handle requires swapIndexId to be set");
    }

    SwaptionVolEntry out = base;
    auto qlVol = std::make_shared<SwaptionSabrParamsCube>(
        base.referenceDate,
        base.calendar,
        base.businessDayConvention,
        base.dayCounter,
        base.qlVolType,
        base.displacement,
        base.expiries,
        base.tenors,
        base.sabrAlpha,
        base.sabrBeta,
        base.sabrRho,
        base.sabrNu,
        atmForwardsFlat);

    out.handle = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(qlVol);
    out.atmForwardsFlat = atmForwardsFlat;
    return out;
}

// =============================================================================
// YoY Inflation Optionlet Vol Parser
// =============================================================================

YoYOptionletVolEntry parseYoYOptionletVol(const quantra::VolSurfaceSpec* spec) {
    if (!spec || !spec->id()) {
        QUANTRA_INVALID_ARGUMENT("VolSurfaceSpec or id is null");
    }
    std::string id = spec->id()->str();

    const auto* p = spec->payload_as_YoYOptionletVolSpec();
    if (!p) {
        QUANTRA_INVALID_ARGUMENT("YoYOptionletVolSpec payload missing for vol id: " + id);
    }
    if (!p->constant_vol().has_value()) {
        QUANTRA_INVALID_ARGUMENT("YoYOptionletVolSpec.constant_vol is required for vol id: " + id);
    }
    if (!p->vol_type().has_value()) {
        QUANTRA_INVALID_ARGUMENT("YoYOptionletVolSpec.vol_type is required for vol id: " + id);
    }
    if (!p->day_counter().has_value()) {
        QUANTRA_INVALID_ARGUMENT("YoYOptionletVolSpec.day_counter is required for vol id: " + id);
    }
    if (!p->calendar().has_value()) {
        QUANTRA_INVALID_ARGUMENT("YoYOptionletVolSpec.calendar is required for vol id: " + id);
    }
    if (!p->business_day_convention().has_value()) {
        QUANTRA_INVALID_ARGUMENT(
            "YoYOptionletVolSpec.business_day_convention is required for vol id: " + id);
    }
    if (!p->observation_lag()) {
        QUANTRA_INVALID_ARGUMENT("YoYOptionletVolSpec.observation_lag is required for vol id: " + id);
    }

    YoYOptionletVolEntry entry;
    entry.constantVol = p->constant_vol().value();
    switch (p->vol_type().value()) {
        case quantra::enums::YoYInflationCapFloorEngineType_Black:
            entry.engineKind = YoYInflationEngineKind::Black;
            break;
        case quantra::enums::YoYInflationCapFloorEngineType_UnitDisplacedBlack:
            entry.engineKind = YoYInflationEngineKind::UnitDisplacedBlack;
            break;
        case quantra::enums::YoYInflationCapFloorEngineType_Bachelier:
            entry.engineKind = YoYInflationEngineKind::Bachelier;
            break;
        default:
            QUANTRA_INVALID_ARGUMENT(
                "Unknown YoYInflationCapFloorEngineType for vol id: " + id);
    }
    entry.dayCounter = DayCounterToQL(p->day_counter().value());
    entry.calendar = CalendarToQL(p->calendar().value());
    entry.businessDayConvention = ConventionToQL(p->business_day_convention().value());
    entry.observationLag = requirePeriod(
        p->observation_lag(), "ConstantYoYOptionletVolSpec.observation_lag");
    return entry;
}

} // namespace quantra
