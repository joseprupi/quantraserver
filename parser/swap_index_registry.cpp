#include "swap_index_registry.h"

namespace quantra {

SwapIndexRegistry SwapIndexRegistryBuilder::build(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::SwapIndexDef>>* defs,
    const IndexRegistry& indices) const {
    SwapIndexRegistry reg;
    if (!defs) return reg;

    for (auto it = defs->begin(); it != defs->end(); ++it) {
        const auto* d = *it;
        if (!d || !d->id()) {
            QUANTRA_ERROR("SwapIndexDef.id is required");
        }
        if (!d->float_index_id()) {
            QUANTRA_ERROR("SwapIndexDef.float_index_id is required");
        }
        SwapIndexRuntime r;
        r.kind = d->kind();
        r.spotDays = d->spot_days();
        r.calendar = CalendarToQL(d->calendar());
        r.bdc = ConventionToQL(d->business_day_convention());
        r.endOfMonth = d->end_of_month();
        r.floatIndexId = d->float_index_id()->str();

        // Validate index kind against referenced index definition.
        if (r.kind == quantra::SwapIndexKind_IborSwapIndex) {
            indices.getIbor(r.floatIndexId);
        } else {
            indices.getOvernight(r.floatIndexId);
        }

        if (!d->fixed_leg()) {
            QUANTRA_ERROR("SwapIndexDef.fixed_leg is required for id: " + d->id()->str());
        }
        const auto* f = d->fixed_leg();
        r.fixedFrequency = FrequencyToQL(f->fixed_frequency());
        r.fixedDayCounter = DayCounterToQL(f->fixed_day_counter());
        r.fixedCalendar = CalendarToQL(f->fixed_calendar());
        r.fixedBdc = ConventionToQL(f->fixed_bdc());
        r.fixedTermBdc = ConventionToQL(f->fixed_term_bdc());
        r.fixedDateRule = DateGenerationToQL(f->fixed_date_rule());
        r.fixedEom = f->fixed_eom();
        if (r.calendar != r.fixedCalendar || r.bdc != r.fixedBdc || r.endOfMonth != r.fixedEom) {
            QUANTRA_ERROR(
                "SwapIndexDef top-level calendar/business_day_convention/end_of_month must match fixed_leg for id: " +
                d->id()->str());
        }

        if (!d->float_leg()) {
            QUANTRA_ERROR("SwapIndexDef.float_leg is required for id: " + d->id()->str());
        }
        const auto* fl = d->float_leg();
        r.floatTenor = QuantLib::Period(
            fl->float_tenor_number(),
            TimeUnitToQL(fl->float_tenor_time_unit()));
        r.floatCalendar = CalendarToQL(fl->float_calendar());
        r.floatBdc = ConventionToQL(fl->float_bdc());
        r.floatTermBdc = ConventionToQL(fl->float_term_bdc());
        r.floatDateRule = DateGenerationToQL(fl->float_date_rule());
        r.floatEom = fl->float_eom();

        if (r.spotDays < 0) {
            QUANTRA_ERROR("SwapIndexDef.spot_days must be >= 0 for id: " + d->id()->str());
        }

        if (r.kind == quantra::SwapIndexKind_OisSwapIndex) {
            auto on = indices.getOvernight(r.floatIndexId);
            if (on->fixingCalendar() != r.fixedCalendar || on->fixingCalendar() != r.floatCalendar) {
                QUANTRA_ERROR(
                    "OIS swap index calendars must match overnight index calendar for id: " + d->id()->str());
            }
            if (r.fixedBdc != r.floatBdc) {
                QUANTRA_ERROR(
                    "OIS swap index requires fixed_leg.fixed_bdc == float_leg.float_bdc for id: " +
                    d->id()->str());
            }
        }
        reg.add(d->id()->str(), r);
    }
    return reg;
}

} // namespace quantra
