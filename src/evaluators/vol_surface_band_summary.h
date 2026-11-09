#ifndef QUANTRA_VOL_SURFACE_BAND_SUMMARY_H
#define QUANTRA_VOL_SURFACE_BAND_SUMMARY_H

/**
 * Summary of quote-level errors by moneyness band x maturity bucket, shared
 * by the fit and the simulation tests of `/validate-vol-surface`.
 *
 * Edges are strictly increasing, finite and > 0. With n edges an axis has
 * n + 1 cells: (-inf, e_0), [e_0, e_1), ..., [e_{n-1}, +inf). A quote falls in
 * the cell whose lower bound is <= its value and whose upper bound is above
 * it. Only cells with at least one quote are returned, ordered by maturity
 * cell then moneyness cell. Per cell: count, quotes inside their bid/ask,
 * and over the quotes that carry a vol error the root mean square and the
 * largest absolute value (absent when none does).
 *
 * Plain C++ (no FlatBuffers, no QuantLib).
 */

#include <optional>
#include <string>
#include <vector>

namespace quantra {

struct VolSurfaceBandEdges {
    std::vector<double> moneyness;
    std::vector<double> maturities;
};

/// K / F: 0.8, 0.9, 0.95, 1.05, 1.1, 1.2; years: 1/12, 0.25, 0.5, 1, 2.
VolSurfaceBandEdges defaultBandEdges();

/// 400 naming `path` + ".moneyness" / ".maturities" when an axis is not
/// strictly increasing, finite and > 0. Empty axes are replaced by the
/// defaults (returned).
VolSurfaceBandEdges resolveBandEdges(const VolSurfaceBandEdges& requested, const std::string& path);

struct BandPoint {
    double moneyness = 0.0;
    double time = 0.0;
    bool inside = false;
    std::optional<double> volError;
};

struct BandCell {
    std::optional<double> moneynessMin, moneynessMax;
    std::optional<double> timeMin, timeMax;
    unsigned count = 0;
    unsigned inside = 0;
    std::optional<double> rmsVolError;
    std::optional<double> maxAbsVolError;
};

/// Index of the cell of `x` on an axis with `edges` (0 .. edges.size()).
std::size_t bandIndex(const std::vector<double>& edges, double x);

std::vector<BandCell> summarizeBands(
    const std::vector<BandPoint>& points, const VolSurfaceBandEdges& edges);

} // namespace quantra

#endif // QUANTRA_VOL_SURFACE_BAND_SUMMARY_H
