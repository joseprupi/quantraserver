#include "vol_surface_band_summary.h"

#include "error.h"

#include <algorithm>
#include <cmath>

namespace quantra {

VolSurfaceBandEdges defaultBandEdges() {
    return VolSurfaceBandEdges{{0.8, 0.9, 0.95, 1.05, 1.1, 1.2}, {1.0 / 12.0, 0.25, 0.5, 1.0, 2.0}};
}

namespace {

void checkAxis(const std::vector<double>& e, const std::string& path) {
    for (std::size_t i = 0; i < e.size(); ++i) {
        if (!std::isfinite(e[i]) || !(e[i] > 0.0)) {
            QUANTRA_INVALID_ARGUMENT(path + "[" + std::to_string(i) + "] must be finite and > 0");
        }
        if (i > 0 && !(e[i] > e[i - 1])) {
            QUANTRA_INVALID_ARGUMENT(path + " must be strictly increasing");
        }
    }
}

} // namespace

VolSurfaceBandEdges resolveBandEdges(
    const VolSurfaceBandEdges& requested, const std::string& path) {
    const VolSurfaceBandEdges d = defaultBandEdges();
    VolSurfaceBandEdges out;
    out.moneyness = requested.moneyness.empty() ? d.moneyness : requested.moneyness;
    out.maturities = requested.maturities.empty() ? d.maturities : requested.maturities;
    checkAxis(out.moneyness, path + ".moneyness");
    checkAxis(out.maturities, path + ".maturities");
    return out;
}

std::size_t bandIndex(const std::vector<double>& edges, double x) {
    return static_cast<std::size_t>(
        std::upper_bound(edges.begin(), edges.end(), x) - edges.begin());
}

std::vector<BandCell> summarizeBands(
    const std::vector<BandPoint>& points, const VolSurfaceBandEdges& edges) {
    const std::size_t nm = edges.moneyness.size() + 1, nt = edges.maturities.size() + 1;
    struct Acc {
        unsigned count = 0, inside = 0, withVol = 0;
        double sumSq = 0.0, maxAbs = 0.0;
    };
    std::vector<Acc> acc(nm * nt);
    for (const auto& p : points) {
        Acc& a =
            acc[bandIndex(edges.maturities, p.time) * nm + bandIndex(edges.moneyness, p.moneyness)];
        ++a.count;
        if (p.inside) ++a.inside;
        if (p.volError) {
            ++a.withVol;
            a.sumSq += *p.volError * *p.volError;
            a.maxAbs = std::max(a.maxAbs, std::fabs(*p.volError));
        }
    }
    std::vector<BandCell> out;
    for (std::size_t it = 0; it < nt; ++it) {
        for (std::size_t im = 0; im < nm; ++im) {
            const Acc& a = acc[it * nm + im];
            if (a.count == 0) continue;
            BandCell c;
            if (im > 0) c.moneynessMin = edges.moneyness[im - 1];
            if (im < nm - 1) c.moneynessMax = edges.moneyness[im];
            if (it > 0) c.timeMin = edges.maturities[it - 1];
            if (it < nt - 1) c.timeMax = edges.maturities[it];
            c.count = a.count;
            c.inside = a.inside;
            if (a.withVol > 0) {
                c.rmsVolError = std::sqrt(a.sumSq / a.withVol);
                c.maxAbsVolError = a.maxAbs;
            }
            out.push_back(c);
        }
    }
    return out;
}

} // namespace quantra
