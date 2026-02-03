/**
 * Vol Surface Parsers Implementation
 * 
 * All vol parsers in one file for simpler build integration.
 */

#include "vol_surface_parsers.h"

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

void validateIrVolBase(const quantra::IrVolBaseSpec* b, const std::string& id) {
    if (!b) {
        QUANTRA_ERROR("IrVolBaseSpec missing for vol id: " + id);
    }
    if (!b->reference_date()) {
        QUANTRA_ERROR("reference_date required for vol id: " + id);
    }
    if (b->constant_vol() <= 0.0) {
        QUANTRA_ERROR("constant_vol must be > 0 for vol id: " + id);
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

} // anonymous namespace

// =============================================================================
// Optionlet Vol Parser (for Caps/Floors)
// =============================================================================

OptionletVolEntry parseOptionletVol(const quantra::VolSurfaceSpec* spec) {
    if (!spec || !spec->id()) {
        QUANTRA_ERROR("VolSurfaceSpec or id is null");
    }
    std::string id = spec->id()->str();
    
    auto* payload = spec->payload_as_OptionletVolSpec();
    if (!payload) {
        QUANTRA_ERROR("OptionletVolSpec payload missing for vol id: " + id);
    }

    const auto* b = payload->base();
    validateIrVolBase(b, id);

    QuantLib::Date ref = DateToQL(b->reference_date()->str());
    QuantLib::Calendar cal = CalendarToQL(b->calendar());
    QuantLib::BusinessDayConvention bdc = ConventionToQL(b->business_day_convention());
    QuantLib::DayCounter dc = DayCounterToQL(b->day_counter());
    double vol = b->constant_vol();
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

SwaptionVolEntry parseSwaptionVol(const quantra::VolSurfaceSpec* spec) {
    if (!spec || !spec->id()) {
        QUANTRA_ERROR("VolSurfaceSpec or id is null");
    }
    std::string id = spec->id()->str();
    
    auto* payload = spec->payload_as_SwaptionVolSpec();
    if (!payload) {
        QUANTRA_ERROR("SwaptionVolSpec payload missing for vol id: " + id);
    }

    const auto* b = payload->base();
    validateIrVolBase(b, id);

    QuantLib::Date ref = DateToQL(b->reference_date()->str());
    QuantLib::Calendar cal = CalendarToQL(b->calendar());
    QuantLib::BusinessDayConvention bdc = ConventionToQL(b->business_day_convention());
    QuantLib::DayCounter dc = DayCounterToQL(b->day_counter());
    double vol = b->constant_vol();
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
    entry.dayCounter = dc;
    
    return entry;
}

// =============================================================================
// Black Vol Parser (for Equity/FX)
// =============================================================================

BlackVolEntry parseBlackVol(const quantra::VolSurfaceSpec* spec) {
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
    double vol = b->constant_vol();

    auto qlVol = std::make_shared<QuantLib::BlackConstantVol>(ref, cal, vol, dc);

    BlackVolEntry entry;
    entry.handle = QuantLib::Handle<QuantLib::BlackVolTermStructure>(qlVol);
    entry.constantVol = vol;
    entry.referenceDate = ref;
    entry.calendar = cal;
    entry.dayCounter = dc;
    
    return entry;
}

} // namespace quantra
