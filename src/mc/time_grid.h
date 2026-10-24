#ifndef QUANTRA_MC_TIME_GRID_H
#define QUANTRA_MC_TIME_GRID_H

/**
 * Simulation time grid for the shared Monte Carlo core.
 *
 * Time axis: t = Actual/365 (Fixed) year fraction from the valuation date,
 * i.e. t = (date - valuation) / 365. The grid is what the path engine steps
 * over; each process's curves and vol are evaluated at these t on the
 * curves' own day counters (as QuantLib's MC engines do), so a caller that
 * wants the grid times to coincide with the curves' times builds the curves
 * on Actual/365 (Fixed) as well.
 *
 * Without `stepsPerYear` the grid is the mandatory times only: with every
 * asset exact-step capable (basket_model.h) one lognormal step per
 * observation date is exact. With `stepsPerYear` every interval between
 * consecutive distinct grid times (starting at 0) is split into
 * ceil(length * stepsPerYear) equal steps, so the spacing is <= 1 /
 * stepsPerYear years everywhere and every mandatory date stays on the grid.
 * (QuantLib's TimeGrid(begin, end, steps) rounds the step count to the
 * nearest integer, which can leave a spacing above 1 / stepsPerYear; the
 * refined times are therefore generated here and passed to the
 * mandatory-times constructor, which adds no points.)
 */

#include <ql/time/date.hpp>
#include <ql/timegrid.hpp>
#include <ql/types.hpp>

#include <map>
#include <optional>
#include <vector>

namespace quantra { namespace mc {

struct SimulationGrid {
    /// The grid: times()[0] == 0 (the valuation date), strictly increasing.
    QuantLib::TimeGrid grid;
    /// Grid index of each mandatory date, in the order the dates were given
    /// (a repeated date maps to the same index; the valuation date itself to 0).
    std::vector<QuantLib::Size> mandatoryIndices;
    /// Mandatory times in that same order.
    std::vector<QuantLib::Time> mandatoryTimes;

    /// Grid index of a mandatory date; 400 naming the date when it is not one
    /// of the mandatory dates.
    QuantLib::Size indexOf(const QuantLib::Date& date) const;

    /// Number of grid points (time steps + 1).
    QuantLib::Size size() const { return grid.size(); }

    std::map<QuantLib::Date, QuantLib::Size> dateIndex;
};

/**
 * @param valuation       date of t = 0
 * @param mandatoryDates  dates that must be grid points (observation dates),
 *                        sorted non-decreasing, none before `valuation`, at
 *                        least one after it; a date equal to `valuation` is
 *                        index 0. Violations are 400s naming the rule.
 * @param stepsPerYear    absent: grid = mandatory times only; present (> 0,
 *                        else 400): refined as described above.
 */
SimulationGrid BuildSimulationGrid(
    const QuantLib::Date& valuation,
    const std::vector<QuantLib::Date>& mandatoryDates,
    std::optional<QuantLib::Size> stepsPerYear);

}} // namespace quantra::mc

#endif // QUANTRA_MC_TIME_GRID_H
