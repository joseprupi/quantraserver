#include "time_grid.h"

#include "error.h"

#include <ql/time/daycounters/actual365fixed.hpp>

#include <cmath>
#include <sstream>

namespace quantra { namespace mc {

QuantLib::Size SimulationGrid::indexOf(const QuantLib::Date& date) const {
    const auto it = dateIndex.find(date);
    if (it == dateIndex.end()) {
        std::ostringstream msg;
        msg << "simulation grid: " << date << " is not a mandatory date of the grid";
        QUANTRA_INVALID_ARGUMENT(msg.str());
    }
    return it->second;
}

SimulationGrid BuildSimulationGrid(
    const QuantLib::Date& valuation,
    const std::vector<QuantLib::Date>& mandatoryDates,
    std::optional<QuantLib::Size> stepsPerYear) {
    if (mandatoryDates.empty()) {
        QUANTRA_INVALID_ARGUMENT("simulation grid: at least one mandatory date is required");
    }
    if (stepsPerYear.has_value() && *stepsPerYear == 0) {
        QUANTRA_INVALID_ARGUMENT("simulation grid: steps_per_year must be > 0");
    }

    const QuantLib::Actual365Fixed dc;
    SimulationGrid out;
    out.mandatoryTimes.reserve(mandatoryDates.size());

    // Distinct mandatory times, strictly increasing, starting at 0.
    std::vector<QuantLib::Time> distinct{0.0};
    for (std::size_t i = 0; i < mandatoryDates.size(); ++i) {
        const auto& d = mandatoryDates[i];
        if (d < valuation) {
            std::ostringstream msg;
            msg << "simulation grid: mandatory date " << d << " is before the valuation date "
                << valuation;
            QUANTRA_INVALID_ARGUMENT(msg.str());
        }
        if (i > 0 && d < mandatoryDates[i - 1]) {
            std::ostringstream msg;
            msg << "simulation grid: mandatory dates must be sorted, " << d << " follows "
                << mandatoryDates[i - 1];
            QUANTRA_INVALID_ARGUMENT(msg.str());
        }
        const QuantLib::Time t = dc.yearFraction(valuation, d);
        out.mandatoryTimes.push_back(t);
        if (t > distinct.back()) {
            distinct.push_back(t);
        }
    }
    if (distinct.size() < 2) {
        QUANTRA_INVALID_ARGUMENT(
            "simulation grid: at least one mandatory date after the valuation date is required");
    }

    // Grid times: the distinct mandatory times, each interval refined to
    // ceil(length * stepsPerYear) equal steps when stepsPerYear is given.
    std::vector<QuantLib::Time> times{0.0};
    std::vector<QuantLib::Size> distinctIndex{0};
    for (std::size_t k = 1; k < distinct.size(); ++k) {
        const QuantLib::Time a = distinct[k - 1];
        const QuantLib::Time b = distinct[k];
        QuantLib::Size n = 1;
        if (stepsPerYear.has_value()) {
            const double raw = (b - a) * static_cast<double>(*stepsPerYear);
            n = static_cast<QuantLib::Size>(std::ceil(raw - 1e-9));
            if (n < 1) {
                n = 1;
            }
        }
        for (QuantLib::Size s = 1; s < n; ++s) {
            times.push_back(a + (b - a) * static_cast<double>(s) / static_cast<double>(n));
        }
        times.push_back(b);
        distinctIndex.push_back(times.size() - 1);
    }

    out.grid = QuantLib::TimeGrid(times.begin(), times.end());
    if (out.grid.size() != times.size()) {
        QUANTRA_ERROR("simulation grid: QuantLib TimeGrid merged or added points unexpectedly");
    }

    // Map every mandatory date to its grid index.
    std::size_t k = 0;
    for (std::size_t i = 0; i < mandatoryDates.size(); ++i) {
        const QuantLib::Time t = out.mandatoryTimes[i];
        while (distinct[k] < t) {
            ++k;
        }
        const QuantLib::Size idx = distinctIndex[k];
        out.mandatoryIndices.push_back(idx);
        out.dateIndex[mandatoryDates[i]] = idx;
    }
    return out;
}

}} // namespace quantra::mc
