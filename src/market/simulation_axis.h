#ifndef QUANTRA_SIMULATION_AXIS_H
#define QUANTRA_SIMULATION_AXIS_H

/**
 * A yield curve put on the simulation's time axis.
 *
 * A Monte Carlo simulation (structured notes, /validate-vol-surface) and the
 * local-vol arbitrage scan measure time in Actual/365 (Fixed) years from the
 * valuation date (src/mc/time_grid.cpp) and read curves at those times.
 * QuantLib reads a time on the curve's own day counter from the curve's own
 * reference date, so a curve on another day counter (Actual/360, 30/360,
 * Actual/Actual) or another reference date would return the discount factor
 * of another date.
 *
 * SimulationAxisCurve answers a read at time t with the wrapped curve's
 * discount factor at the date t denotes: t * 365 days after the valuation
 * date. A whole number of days is the wrapped curve read at that date; a
 * fraction of a day interpolates the wrapped curve's own time linearly
 * between the two neighbouring dates (exact for Actual/360). A date before
 * the wrapped curve's reference date reads its discount factor at that
 * reference date (1). The view observes the wrapped curve: a change of its
 * quotes or a relink of its handle notifies the view's observers.
 *
 * Vols are not converted: a vol read on that axis must already be on it
 * (RequireVolOnSimulationAxis).
 */

#include <ql/handle.hpp>
#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/date.hpp>

#include <string>
#include <vector>

namespace quantra {

class SimulationAxisCurve : public QuantLib::YieldTermStructure {
public:
    SimulationAxisCurve(
        QuantLib::Handle<QuantLib::YieldTermStructure> inner, const QuantLib::Date& valuationDate);

    QuantLib::Date maxDate() const override;
    QuantLib::Calendar calendar() const override;
    QuantLib::Natural settlementDays() const override;

    /// The wrapped curve's own time for the simulation time t.
    QuantLib::Time innerTime(QuantLib::Time t) const;

    void update() override;

protected:
    QuantLib::DiscountFactor discountImpl(QuantLib::Time t) const override;

private:
    /// The wrapped curve's time at a whole number of days after the
    /// valuation date.
    QuantLib::Time innerTimeAtDay(long day) const;
    void tabulate();

    QuantLib::Handle<QuantLib::YieldTermStructure> inner_;
    /// innerTimeAtDay of the first days, rebuilt on every update.
    std::vector<QuantLib::Time> dayTimes_;
};

/// True when `curve` already reads times as the simulation does: day counter
/// Actual/365 (Fixed) and reference date `valuationDate`.
bool IsOnSimulationAxis(
    const QuantLib::YieldTermStructure& curve, const QuantLib::Date& valuationDate);

/// `curve` on the simulation's time axis: the same handle when it is empty
/// or IsOnSimulationAxis, else a SimulationAxisCurve on it (extrapolating when
/// the curve does).
QuantLib::Handle<QuantLib::YieldTermStructure> OnSimulationAxis(
    const QuantLib::Handle<QuantLib::YieldTermStructure>& curve,
    const QuantLib::Date& valuationDate);

/// 400 unless `vol` reads times as the simulation does: day counter
/// Actual/365 (Fixed) and reference date `valuationDate`. `volId` names the
/// vol in the message.
void RequireVolOnSimulationAxis(
    const QuantLib::BlackVolTermStructure& vol,
    const QuantLib::Date& valuationDate,
    const std::string& volId);

} // namespace quantra

#endif // QUANTRA_SIMULATION_AXIS_H
