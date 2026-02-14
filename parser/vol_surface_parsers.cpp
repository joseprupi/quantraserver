/**
 * Vol Surface Parsers Implementation
 * 
 * All vol parsers in one file for simpler build integration.
 */

#include "vol_surface_parsers.h"

#include <ql/termstructures/volatility/swaption/swaptionvolmatrix.hpp>
#include <ql/termstructures/volatility/interpolatedsmilesection.hpp>
#include <ql/math/interpolations/linearinterpolation.hpp>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <iostream>
#include <cctype>

namespace quantra {

// =============================================================================
// Utility Functions
// =============================================================================

QuantLib::VolatilityType toQlVolType(quantra::enums::VolatilityType t) {
    switch (t) {
        case quantra::enums::VolatilityType_Normal:
            return QuantLib::Normal;
            
        case quantra::enums::VolatilityType_Lognormal:
        case quantra::enums::VolatilityType_ShiftedLognormal:
            return QuantLib::ShiftedLognormal;
            
        default:
            QUANTRA_ERROR("Unknown VolatilityType enum value: " + std::to_string(static_cast<int>(t)));
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

void validateIrVolBaseCommon(const quantra::IrVolBaseSpec* b, const std::string& id) {
    if (!b) {
        QUANTRA_ERROR("IrVolBaseSpec missing for vol id: " + id);
    }
    if (!b->reference_date()) {
        QUANTRA_ERROR("reference_date required for vol id: " + id);
    }

    auto volType = b->volatility_type();
    double disp = b->displacement();
    
    if (volType == quantra::enums::VolatilityType_ShiftedLognormal && disp <= 0.0) {
        QUANTRA_ERROR("ShiftedLognormal requires displacement > 0 for vol id: " + id);
    }
    if (volType == quantra::enums::VolatilityType_Lognormal && disp != 0.0) {
        QUANTRA_ERROR("Lognormal requires displacement == 0 for vol id: " + id);
    }
}

void validateIrVolBaseConstant(const quantra::IrVolBaseSpec* b, const std::string& id) {
    validateIrVolBaseCommon(b, id);
    bool hasQuote = b->quote_id() && !b->quote_id()->str().empty();
    if (!hasQuote && b->constant_vol() <= 0.0) {
        QUANTRA_ERROR("constant_vol must be > 0 (or quote_id provided) for vol id: " + id);
    }
}

void validateSupportedInterpolator(quantra::enums::Interpolator interp, const std::string& label, const std::string& id) {
    if (interp != quantra::enums::Interpolator_Linear) {
        QUANTRA_ERROR(label + " only supports Linear interpolator for vol id: " + id);
    }
}

void validateBlackVolBase(const quantra::BlackVolBaseSpec* b, const std::string& id) {
    if (!b) {
        QUANTRA_ERROR("BlackVolBaseSpec missing for vol id: " + id);
    }
    if (!b->reference_date()) {
        QUANTRA_ERROR("reference_date required for vol id: " + id);
    }
    if (b->constant_vol() <= 0.0) {
        QUANTRA_ERROR("constant_vol must be > 0 for vol id: " + id);
    }
}

QuantLib::Period toQlPeriod(const quantra::PeriodSpec* p) {
    if (!p) {
        QUANTRA_ERROR("PeriodSpec is null");
    }
    QuantLib::TimeUnit unit;
    switch (p->tenor_time_unit()) {
        case quantra::enums::TimeUnit_Days:
            unit = QuantLib::Days;
            break;
        case quantra::enums::TimeUnit_Weeks:
            unit = QuantLib::Weeks;
            break;
        case quantra::enums::TimeUnit_Months:
            unit = QuantLib::Months;
            break;
        case quantra::enums::TimeUnit_Years:
            unit = QuantLib::Years;
            break;
        default:
            QUANTRA_ERROR("Unknown TimeUnit for PeriodSpec");
    }
    return QuantLib::Period(p->tenor_number(), unit);
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
        QUANTRA_ERROR("QuoteMatrix2D missing for vol id: " + id);
    }
    if (m->n_rows() != nRows || m->n_cols() != nCols) {
        QUANTRA_ERROR("QuoteMatrix2D dims mismatch for vol id: " + id);
    }
    int expected = nRows * nCols;
    if (!m->values() || static_cast<int>(m->values()->size()) != expected) {
        QUANTRA_ERROR("QuoteMatrix2D values length mismatch for vol id: " + id);
    }
    if (m->quote_ids() && static_cast<int>(m->quote_ids()->size()) != expected) {
        QUANTRA_ERROR("QuoteMatrix2D quote_ids length mismatch for vol id: " + id);
    }
}

void validateTensor3D(const quantra::QuoteTensor3D* t, int n1, int n2, int n3, const std::string& id) {
    if (!t) {
        QUANTRA_ERROR("QuoteTensor3D missing for vol id: " + id);
    }
    if (t->n_1() != n1 || t->n_2() != n2 || t->n_3() != n3) {
        QUANTRA_ERROR("QuoteTensor3D dims mismatch for vol id: " + id);
    }
    int expected = n1 * n2 * n3;
    if (!t->values() || static_cast<int>(t->values()->size()) != expected) {
        QUANTRA_ERROR("QuoteTensor3D values length mismatch for vol id: " + id);
    }
    if (t->quote_ids() && static_cast<int>(t->quote_ids()->size()) != expected) {
        QUANTRA_ERROR("QuoteTensor3D quote_ids length mismatch for vol id: " + id);
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
            QUANTRA_ERROR("SwaptionVolSmileCubeSpec strikes must be sorted ascending");
        }
        auto dup = std::adjacent_find(strikes_.begin(), strikes_.end(), [](double a, double b) { return a >= b; });
        if (dup != strikes_.end()) {
            QUANTRA_ERROR("SwaptionVolSmileCubeSpec strikes must be strictly increasing");
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
            QUANTRA_ERROR("SwaptionVolSmileCubeSpec atm_forwards matrix size mismatch");
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
            QUANTRA_ERROR("Smile cube strikes are empty");
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
        size_t hi = 1;
        while (hi < grid.size() && grid[hi] < x) hi++;
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
                QUANTRA_ERROR("SpreadFromATM smile cube requires ATM forwards (server-computed or provided)");
            }
            return strikes_.empty() ? 0.0 : strikes_[strikes_.size() / 2];
        }
        return atmForwards_[idx2d(i, j)];
    }

    double bilinearAtm(double tExp, double tTen) const {
        if (atmForwards_.empty()) {
            if (strikeKind_ == quantra::enums::SwaptionStrikeKind_SpreadFromATM) {
                QUANTRA_ERROR("SpreadFromATM smile cube requires ATM forwards (server-computed or provided)");
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
        QUANTRA_ERROR("constant_vol must be > 0 for vol id: " + id);
    }
    return inlineValue;
}

} // anonymous namespace

// =============================================================================
// Optionlet Vol Parser (for Caps/Floors)
// =============================================================================

OptionletVolEntry parseOptionletVol(const quantra::VolSurfaceSpec* spec, const QuoteRegistry* quotes) {
    if (!spec || !spec->id()) {
        QUANTRA_ERROR("VolSurfaceSpec or id is null");
    }
    std::string id = spec->id()->str();
    
    auto* payload = spec->payload_as_OptionletVolSpec();
    if (!payload) {
        QUANTRA_ERROR("OptionletVolSpec payload missing for vol id: " + id);
    }

    const auto* b = payload->base();
    validateIrVolBaseConstant(b, id);

    QuantLib::Date ref = DateToQL(b->reference_date()->str());
    QuantLib::Calendar cal = CalendarToQL(b->calendar());
    QuantLib::BusinessDayConvention bdc = ConventionToQL(b->business_day_convention());
    QuantLib::DayCounter dc = DayCounterToQL(b->day_counter());
    double vol = resolveVolValue(b->constant_vol(), b->quote_id(), quotes, id);
    double disp = b->displacement();
    QuantLib::VolatilityType qlType = toQlVolType(b->volatility_type());

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
    entry.dayCounter = dc;
    
    return entry;
}

// =============================================================================
// Swaption Vol Parser
// =============================================================================

SwaptionVolEntry parseSwaptionVol(const quantra::VolSurfaceSpec* spec, const QuoteRegistry* quotes) {
    if (!spec || !spec->id()) {
        QUANTRA_ERROR("VolSurfaceSpec or id is null");
    }
    std::string id = spec->id()->str();

    auto* wrapper = spec->payload_as_SwaptionVolSpec();
    if (!wrapper) {
        QUANTRA_ERROR("SwaptionVolSpec payload missing for vol id: " + id);
    }

    switch (wrapper->payload_type()) {
        case quantra::SwaptionVolPayload_SwaptionVolConstantSpec: {
            auto* payload = wrapper->payload_as_SwaptionVolConstantSpec();
            if (!payload || !payload->base()) {
                QUANTRA_ERROR("SwaptionVolConstantSpec base missing for vol id: " + id);
            }
            const auto* b = payload->base();
            validateIrVolBaseConstant(b, id);

            QuantLib::Date ref = DateToQL(b->reference_date()->str());
            QuantLib::Calendar cal = CalendarToQL(b->calendar());
            QuantLib::BusinessDayConvention bdc = ConventionToQL(b->business_day_convention());
            QuantLib::DayCounter dc = DayCounterToQL(b->day_counter());
            double vol = resolveVolValue(b->constant_vol(), b->quote_id(), quotes, id);
            double disp = b->displacement();
            QuantLib::VolatilityType qlType = toQlVolType(b->volatility_type());

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
            return entry;
        }

        case quantra::SwaptionVolPayload_SwaptionVolAtmMatrixSpec: {
            auto* payload = wrapper->payload_as_SwaptionVolAtmMatrixSpec();
            if (!payload || !payload->base()) {
                QUANTRA_ERROR("SwaptionVolAtmMatrixSpec base missing for vol id: " + id);
            }
            const auto* b = payload->base();
            validateIrVolBaseCommon(b, id);
            validateSupportedInterpolator(payload->expiry_interpolator(), "expiry_interpolator", id);
            validateSupportedInterpolator(payload->tenor_interpolator(), "tenor_interpolator", id);

            QuantLib::Date ref = DateToQL(b->reference_date()->str());
            QuantLib::Calendar cal = CalendarToQL(b->calendar());
            QuantLib::BusinessDayConvention bdc = ConventionToQL(b->business_day_convention());
            QuantLib::DayCounter dc = DayCounterToQL(b->day_counter());
            double disp = b->displacement();
            QuantLib::VolatilityType qlType = toQlVolType(b->volatility_type());

            if (!payload->expiries() || !payload->tenors()) {
                QUANTRA_ERROR("SwaptionVolAtmMatrixSpec expiries/tenors missing for vol id: " + id);
            }
            int nExp = static_cast<int>(payload->expiries()->size());
            int nTen = static_cast<int>(payload->tenors()->size());
            if (nExp <= 0 || nTen <= 0) {
                QUANTRA_ERROR("SwaptionVolAtmMatrixSpec expiries/tenors empty for vol id: " + id);
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
                        QUANTRA_ERROR("SwaptionVolAtmMatrixSpec vol must be > 0 for vol id: " + id);
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
            return entry;
        }

        case quantra::SwaptionVolPayload_SwaptionVolSmileCubeSpec: {
            auto* payload = wrapper->payload_as_SwaptionVolSmileCubeSpec();
            if (!payload || !payload->base()) {
                QUANTRA_ERROR("SwaptionVolSmileCubeSpec base missing for vol id: " + id);
            }
            const auto* b = payload->base();
            validateIrVolBaseCommon(b, id);
            validateSupportedInterpolator(payload->expiry_interpolator(), "expiry_interpolator", id);
            validateSupportedInterpolator(payload->tenor_interpolator(), "tenor_interpolator", id);
            validateSupportedInterpolator(payload->strike_interpolator(), "strike_interpolator", id);

            QuantLib::Date ref = DateToQL(b->reference_date()->str());
            QuantLib::Calendar cal = CalendarToQL(b->calendar());
            QuantLib::BusinessDayConvention bdc = ConventionToQL(b->business_day_convention());
            QuantLib::DayCounter dc = DayCounterToQL(b->day_counter());
            double disp = b->displacement();
            QuantLib::VolatilityType qlType = toQlVolType(b->volatility_type());

            if (!payload->expiries() || !payload->tenors() || !payload->strikes()) {
                QUANTRA_ERROR("SwaptionVolSmileCubeSpec grids missing for vol id: " + id);
            }
            int nExp = static_cast<int>(payload->expiries()->size());
            int nTen = static_cast<int>(payload->tenors()->size());
            int nStr = static_cast<int>(payload->strikes()->size());
            if (nExp <= 0 || nTen <= 0 || nStr <= 0) {
                QUANTRA_ERROR("SwaptionVolSmileCubeSpec grids empty for vol id: " + id);
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
            std::string swapIndexId = payload->swap_index_id() ? payload->swap_index_id()->str() : "";
            if (isBlankString(swapIndexId)) {
                swapIndexId.clear();
            }
            bool allowExternalAtm = payload->allow_external_atm();
            auto strikeKind = payload->strike_kind();
            std::vector<double> atmForwards;
            if (payload->atm_forwards()) {
                if (!allowExternalAtm) {
                    QUANTRA_ERROR(
                        "SwaptionVolSmileCubeSpec atm_forwards requires allow_external_atm=true for vol id: " + id);
                }
                const auto* atm = payload->atm_forwards();
                validateMatrix2D(atm, nExp, nTen, id);
                atmForwards.reserve(nExp * nTen);
                for (int idx = 0; idx < nExp * nTen; ++idx) {
                    atmForwards.push_back(resolveMatrixValue(atm, idx, quotes, id));
                }
            }
            if (strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM &&
                swapIndexId.empty()) {
                QUANTRA_ERROR(
                    "SwaptionVolSmileCubeSpec strike_kind=SpreadFromATM requires swap_index_id for vol id: " + id);
            }
            if (strikeKind == quantra::enums::SwaptionStrikeKind_Absolute && !atmForwards.empty()) {
                QUANTRA_ERROR(
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
                    QUANTRA_ERROR("SwaptionVolSmileCubeSpec vol must be > 0 for vol id: " + id);
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
            entry.swapIndexId = swapIndexId;
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
                QUANTRA_ERROR("SwaptionSabrParamsSpec base missing for vol id: " + id);
            }
            const auto* b = payload->base();
            validateIrVolBaseCommon(b, id);
            QUANTRA_ERROR(
                "SwaptionSabrParamsSpec requires forward-aware cube wiring (e.g. QuantLib SABR cube with swap indices); "
                "pure surface construction without forward access is intentionally not supported for vol id: " + id);
        }

        case quantra::SwaptionVolPayload_SwaptionSabrCalibrateSpec:
            QUANTRA_ERROR("SwaptionSabrCalibrateSpec not implemented yet for vol id: " + id);

        default:
            QUANTRA_ERROR("Unknown SwaptionVolPayload type for vol id: " + id);
    }

    return SwaptionVolEntry();
}

// =============================================================================
// Black Vol Parser (for Equity/FX)
// =============================================================================

BlackVolEntry parseBlackVol(const quantra::VolSurfaceSpec* spec, const QuoteRegistry* quotes) {
    if (!spec || !spec->id()) {
        QUANTRA_ERROR("VolSurfaceSpec or id is null");
    }
    std::string id = spec->id()->str();
    
    auto* payload = spec->payload_as_BlackVolSpec();
    if (!payload) {
        QUANTRA_ERROR("BlackVolSpec payload missing for vol id: " + id);
    }

    const auto* b = payload->base();
    validateBlackVolBase(b, id);

    QuantLib::Date ref = DateToQL(b->reference_date()->str());
    QuantLib::Calendar cal = CalendarToQL(b->calendar());
    QuantLib::DayCounter dc = DayCounterToQL(b->day_counter());
    double vol = resolveVolValue(b->constant_vol(), b->quote_id(), quotes, id);

    auto qlVol = std::make_shared<QuantLib::BlackConstantVol>(ref, cal, vol, dc);

    BlackVolEntry entry;
    entry.handle = QuantLib::Handle<QuantLib::BlackVolTermStructure>(qlVol);
    entry.constantVol = vol;
    entry.referenceDate = ref;
    entry.calendar = cal;
    entry.dayCounter = dc;
    
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
            QUANTRA_ERROR("SABR bump semantics are placeholder-only; runtime bumping is not supported yet");
        }

        case quantra::enums::SwaptionVolKind_SabrCalibrate:
        default:
            QUANTRA_ERROR("Vol bump not supported for this swaption vol kind");
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

} // namespace quantra
