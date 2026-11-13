#include "simulation_axis.h"

#include "date_convert.h"
#include "error.h"

#include <ql/time/daycounters/actual365fixed.hpp>

#include <algorithm>
#include <cmath>

namespace quantra {

SimulationAxisCurve::SimulationAxisCurve(
    QuantLib::Handle<QuantLib::YieldTermStructure> inner, const QuantLib::Date& valuationDate)
    : QuantLib::YieldTermStructure(valuationDate, QuantLib::Calendar(), QuantLib::Actual365Fixed()),
      inner_(std::move(inner)) {
    if (inner_.empty()) {
        QUANTRA_ERROR("simulation axis curve: the wrapped curve handle must be linked");
    }
    registerWith(inner_);
    tabulate();
}

namespace {
// Days of the wrapped curve's times kept in a table (60 years): a read is
// then two lookups; later days are computed on each read.
constexpr long kTabulatedDays = 60 * 366;
} // namespace

void SimulationAxisCurve::tabulate() {
    dayTimes_.resize(kTabulatedDays + 1);
    const QuantLib::Date ref = referenceDate();
    for (long d = 0; d <= kTabulatedDays; ++d)
        dayTimes_[d] = inner_->timeFromReference(ref + static_cast<QuantLib::Date::serial_type>(d));
}

void SimulationAxisCurve::update() {
    // A relinked handle can carry another day counter or reference date.
    if (!inner_.empty()) tabulate();
    QuantLib::YieldTermStructure::update();
}

QuantLib::Time SimulationAxisCurve::innerTimeAtDay(long day) const {
    if (day >= 0 && day <= kTabulatedDays) return dayTimes_[day];
    return inner_->timeFromReference(
        referenceDate() + static_cast<QuantLib::Date::serial_type>(day));
}

QuantLib::Date SimulationAxisCurve::maxDate() const {
    return inner_->maxDate();
}

QuantLib::Calendar SimulationAxisCurve::calendar() const {
    return inner_->calendar();
}

QuantLib::Natural SimulationAxisCurve::settlementDays() const {
    return inner_->settlementDays();
}

QuantLib::Time SimulationAxisCurve::innerTime(QuantLib::Time t) const {
    // t * 365 days after the valuation date; a time within 1e-9 days of a
    // whole day is that day (t = n / 365 does not always round-trip).
    double days = t * 365.0;
    const double whole = std::round(days);
    if (std::fabs(days - whole) < 1.0e-9) days = whole;
    const double lower = std::floor(days);
    const long d0 = static_cast<long>(lower);
    const QuantLib::Time t0 = innerTimeAtDay(d0);
    const double fraction = days - lower;
    if (fraction == 0.0) return t0;
    const QuantLib::Time t1 = innerTimeAtDay(d0 + 1);
    return t0 + fraction * (t1 - t0);
}

QuantLib::DiscountFactor SimulationAxisCurve::discountImpl(QuantLib::Time t) const {
    return inner_->discount(std::max(innerTime(t), 0.0), true);
}

bool IsOnSimulationAxis(
    const QuantLib::YieldTermStructure& curve, const QuantLib::Date& valuationDate) {
    return curve.dayCounter() == QuantLib::Actual365Fixed() &&
           curve.referenceDate() == valuationDate;
}

QuantLib::Handle<QuantLib::YieldTermStructure> OnSimulationAxis(
    const QuantLib::Handle<QuantLib::YieldTermStructure>& curve,
    const QuantLib::Date& valuationDate) {
    if (curve.empty() || IsOnSimulationAxis(**curve, valuationDate)) return curve;
    auto view = std::make_shared<SimulationAxisCurve>(curve, valuationDate);
    if (curve->allowsExtrapolation()) view->enableExtrapolation();
    return QuantLib::Handle<QuantLib::YieldTermStructure>(view);
}

void RequireVolOnSimulationAxis(
    const QuantLib::BlackVolTermStructure& vol,
    const QuantLib::Date& valuationDate,
    const std::string& volId) {
    const char* why =
        " (a structured note and /validate-vol-surface read vols at Actual/365 (Fixed) years from "
        "as_of_date)";
    if (vol.dayCounter() != QuantLib::Actual365Fixed()) {
        QUANTRA_INVALID_ARGUMENT(
            "volatility '" + volId + "': base.day_counter must be Actual365Fixed" + why + ", got " +
            vol.dayCounter().name());
    }
    if (vol.referenceDate() != valuationDate) {
        QUANTRA_INVALID_ARGUMENT(
            "volatility '" + volId + "': base.reference_date must be the as_of_date " +
            DateToIso(valuationDate) + why + ", got " + DateToIso(vol.referenceDate()));
    }
}

} // namespace quantra
