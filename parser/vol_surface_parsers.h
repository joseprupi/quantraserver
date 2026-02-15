#ifndef QUANTRA_VOL_SURFACE_PARSERS_H
#define QUANTRA_VOL_SURFACE_PARSERS_H

/**
 * Vol Surface Parsers
 * 
 * Parsers for typed volatility surfaces. Each vol type has its own parser.
 * 
 * Supported types:
 *   - OptionletVolSpec  -> ConstantOptionletVolatility (caps/floors)
 *   - SwaptionVolSpec   -> ConstantSwaptionVolatility (swaptions)
 *   - BlackVolSpec      -> BlackConstantVol (equity/FX options)
 */

#include <ql/handle.hpp>
#include <ql/termstructures/volatility/optionlet/optionletvolatilitystructure.hpp>
#include <ql/termstructures/volatility/swaption/swaptionvolstructure.hpp>
#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/termstructures/volatility/optionlet/constantoptionletvol.hpp>
#include <ql/termstructures/volatility/swaption/swaptionconstantvol.hpp>
#include <vector>
#include <string>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/time/date.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/daycounter.hpp>

#include "volatility_generated.h"
#include "enums.h"
#include "common.h"
#include "quote_registry.h"

namespace quantra {

// =============================================================================
// Vol Entry structs - store handle + metadata for engine factory validation
// =============================================================================

struct OptionletVolEntry {
    QuantLib::Handle<QuantLib::OptionletVolatilityStructure> handle;
    QuantLib::VolatilityType qlVolType;
    double displacement;
    double constantVol;
    QuantLib::Date referenceDate;
    QuantLib::Calendar calendar;
    quantra::enums::Calendar calendarFb = quantra::enums::Calendar_NullCalendar;
    quantra::enums::BusinessDayConvention businessDayConventionFb =
        quantra::enums::BusinessDayConvention_Following;
    QuantLib::DayCounter dayCounter;
};

struct SwaptionVolEntry {
    QuantLib::Handle<QuantLib::SwaptionVolatilityStructure> handle;
    QuantLib::VolatilityType qlVolType;
    double displacement;
    double constantVol;
    QuantLib::Date referenceDate;
    QuantLib::Calendar calendar;
    QuantLib::BusinessDayConvention businessDayConvention;
    QuantLib::DayCounter dayCounter;
    quantra::enums::SwaptionVolKind volKind = quantra::enums::SwaptionVolKind_Constant;
    std::vector<QuantLib::Period> expiries;
    std::vector<QuantLib::Period> tenors;
    std::vector<double> strikes;
    std::string swapIndexId;
    quantra::enums::SwaptionStrikeKind strikeKind = quantra::enums::SwaptionStrikeKind_Absolute;
    bool allowExternalAtm = false;
    std::vector<double> volsFlat;
    std::vector<double> atmForwardsFlat;
    std::vector<double> sabrAlpha;
    std::vector<double> sabrBeta;
    std::vector<double> sabrRho;
    std::vector<double> sabrNu;
    int nExp = 0;
    int nTen = 0;
    int nStrikes = 0;
};

struct BlackVolEntry {
    QuantLib::Handle<QuantLib::BlackVolTermStructure> handle;
    double constantVol;
    QuantLib::Date referenceDate;
    QuantLib::Calendar calendar;
    QuantLib::DayCounter dayCounter;
};

// =============================================================================
// Parser functions - free functions (not class methods)
// =============================================================================

/**
 * Convert Quantra enum to QuantLib VolatilityType.
 */
QuantLib::VolatilityType toQlVolType(quantra::enums::VolatilityType t);

/**
 * Parse OptionletVolSpec from FlatBuffers into QuantLib structure.
 */
OptionletVolEntry parseOptionletVol(const quantra::VolSurfaceSpec* spec, const QuoteRegistry* quotes = nullptr);

/**
 * Parse SwaptionVolSpec from FlatBuffers into QuantLib structure.
 */
SwaptionVolEntry parseSwaptionVol(const quantra::VolSurfaceSpec* spec, const QuoteRegistry* quotes = nullptr);

/**
 * Build a bumped swaption vol entry (parallel bump).
 */
SwaptionVolEntry bumpSwaptionVolEntry(const SwaptionVolEntry& base, double volBump);

/**
 * Rebuild smile cube entry with server-computed ATM forwards.
 */
SwaptionVolEntry withSwaptionSmileCubeAtm(
    const SwaptionVolEntry& base,
    const std::vector<double>& atmForwardsFlat);

/**
 * Parse BlackVolSpec from FlatBuffers into QuantLib structure.
 */
BlackVolEntry parseBlackVol(const quantra::VolSurfaceSpec* spec, const QuoteRegistry* quotes = nullptr);

} // namespace quantra

#endif // QUANTRA_VOL_SURFACE_PARSERS_H
