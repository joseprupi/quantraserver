#ifndef QUANTRA_SWAPTION_REBUMP_H
#define QUANTRA_SWAPTION_REBUMP_H

/**
 * Shared, FlatBuffers-free configuration for the swaption rebump greeks.
 *
 * The rebump greeks are computed by re-pricing the swaption against bumped /
 * rolled market snapshots. The mapper eagerly pre-builds those snapshots and
 * the pricer differences the resulting NPVs into DV01/gamma/vega/theta. Both
 * sides must agree on the bump magnitudes, so the constants live here, in one
 * place, on the FB-free side of the mapper/pricer boundary (Suite 0).
 */

namespace quantra {

/// Perturbation sizes for the swaption rebump greeks. The defaults reproduce
/// exactly the literal constants the pricer used before snapshots were lifted
/// into the mapper: a 1bp parallel curve bump, a 1bp vol bump, and a 1-day
/// eval-date roll.
struct SwaptionRebumpConfig {
    /// Parallel curve bump in absolute units (1bp) for the DV01/gamma legs.
    double curveBump = 1.0e-4;
    /// Vol bump in absolute units (1bp) for the vega leg.
    double volBump = 1.0e-4;
    /// Eval-date roll in days for the theta leg.
    int rollDays = 1;
};

/// The single source of truth read by both the mapper (to pre-build the bumped
/// and rolled snapshots) and the pricer (to compute the greeks). Keeping it a
/// constexpr instance guarantees the bump magnitude can never drift between the
/// two sides.
inline constexpr SwaptionRebumpConfig kSwaptionRebumpConfig{};

} // namespace quantra

#endif // QUANTRA_SWAPTION_REBUMP_H
