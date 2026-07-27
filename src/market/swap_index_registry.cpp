#include "swap_index_registry.h"

#include "require_field.h"

namespace quantra {

namespace {

QuantLib::Period frequencyToPeriod(QuantLib::Frequency f) {
    switch (f) {
        case QuantLib::Annual: return QuantLib::Period(1, QuantLib::Years);
        case QuantLib::Semiannual: return QuantLib::Period(6, QuantLib::Months);
        case QuantLib::Quarterly: return QuantLib::Period(3, QuantLib::Months);
        case QuantLib::Monthly: return QuantLib::Period(1, QuantLib::Months);
        case QuantLib::Bimonthly: return QuantLib::Period(2, QuantLib::Months);
        case QuantLib::EveryFourthMonth: return QuantLib::Period(4, QuantLib::Months);
        default: return QuantLib::Period(1, QuantLib::Years);
    }
}

} // namespace

std::shared_ptr<QuantLib::SwapIndex> SwapIndexRegistry::getIborSwapIndexWithCurves(
    const std::string& id,
    const QuantLib::Period& tenor,
    const IndexRegistry& indices,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve) const {
    const auto& sidx = get(id);
    if (sidx.kind != quantra::SwapIndexKind_IborSwapIndex) {
        QUANTRA_INVALID_ARGUMENT("Swap index '" + id + "' is not an Ibor swap index");
    }
    if (tenor.length() <= 0) {
        QUANTRA_INVALID_ARGUMENT("CMS swap_tenor must be positive for swap index '" + id + "'");
    }

    auto ibor = indices.getIborWithCurve(sidx.floatIndexId, forwardingCurve);
    QuantLib::Period fixedLegTenor = frequencyToPeriod(sidx.fixedFrequency);

    return std::make_shared<QuantLib::SwapIndex>(
        id,
        tenor,
        static_cast<QuantLib::Natural>(sidx.spotDays),
        ibor->currency(),
        sidx.fixedCalendar,
        fixedLegTenor,
        sidx.fixedBdc,
        sidx.fixedDayCounter,
        ibor,
        discountCurve);
}

SwapIndexRegistry SwapIndexRegistryBuilder::build(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::SwapIndexDef>>* defs,
    const IndexRegistry& indices) const {
    SwapIndexRegistry reg;
    if (!defs) return reg;

    for (auto it = defs->begin(); it != defs->end(); ++it) {
        const auto* d = *it;
        if (!d || !d->id()) {
            QUANTRA_INVALID_ARGUMENT("SwapIndexDef.id is required");
        }
        if (!d->float_index_id()) {
            QUANTRA_INVALID_ARGUMENT("SwapIndexDef.float_index_id is required");
        }
        const std::string sid = d->id()->str();
        SwapIndexRuntime r;
        r.kind = requireEnum(d->kind(), "SwapIndexDef.kind for id: " + sid);
        r.spotDays = requireInt(d->spot_days(), "SwapIndexDef.spot_days for id: " + sid);
        r.calendar = CalendarToQL(
            requireEnum(d->calendar(), "SwapIndexDef.calendar for id: " + sid));
        r.bdc = ConventionToQL(requireEnum(
            d->business_day_convention(),
            "SwapIndexDef.business_day_convention for id: " + sid));
        r.endOfMonth = requireBool(
            d->end_of_month(), "SwapIndexDef.end_of_month for id: " + sid);
        r.floatIndexId = d->float_index_id()->str();

        // Validate index kind against referenced index definition.
        if (r.kind == quantra::SwapIndexKind_IborSwapIndex) {
            indices.getIbor(r.floatIndexId);
        } else {
            indices.getOvernight(r.floatIndexId);
        }

        if (!d->fixed_leg()) {
            QUANTRA_INVALID_ARGUMENT("SwapIndexDef.fixed_leg is required for id: " + d->id()->str());
        }
        const auto* f = d->fixed_leg();
        const auto fixedCalFb =
            requireEnum(f->fixed_calendar(), "SwapIndexFixedLegSpec.fixed_calendar for id: " + sid);
        const auto fixedBdcFb =
            requireEnum(f->fixed_bdc(), "SwapIndexFixedLegSpec.fixed_bdc for id: " + sid);
        r.fixedFrequency = FrequencyToQL(requireEnum(
            f->fixed_frequency(), "SwapIndexFixedLegSpec.fixed_frequency for id: " + sid));
        r.fixedDayCounter = DayCounterToQL(requireEnum(
            f->fixed_day_counter(), "SwapIndexFixedLegSpec.fixed_day_counter for id: " + sid));
        r.fixedCalendar = CalendarToQL(fixedCalFb);
        r.fixedCalendarFb = fixedCalFb;
        r.fixedBdc = ConventionToQL(fixedBdcFb);
        r.fixedBdcFb = fixedBdcFb;
        r.fixedTermBdc = ConventionToQL(requireEnum(
            f->fixed_term_bdc(), "SwapIndexFixedLegSpec.fixed_term_bdc for id: " + sid));
        r.fixedDateRule = DateGenerationToQL(requireEnum(
            f->fixed_date_rule(), "SwapIndexFixedLegSpec.fixed_date_rule for id: " + sid));
        r.fixedEom = requireBool(
            f->fixed_eom(), "SwapIndexFixedLegSpec.fixed_eom for id: " + sid);
        if (r.calendar != r.fixedCalendar || r.bdc != r.fixedBdc || r.endOfMonth != r.fixedEom) {
            QUANTRA_INVALID_ARGUMENT(
                "SwapIndexDef top-level calendar/business_day_convention/end_of_month must match fixed_leg for id: " +
                d->id()->str());
        }

        if (!d->float_leg()) {
            QUANTRA_INVALID_ARGUMENT("SwapIndexDef.float_leg is required for id: " + d->id()->str());
        }
        const auto* fl = d->float_leg();
        if (!fl->float_tenor()) {
            QUANTRA_INVALID_ARGUMENT("SwapIndexDef.float_leg.float_tenor is required for id: " + d->id()->str());
        }
        r.floatTenor = requirePeriod(
            fl->float_tenor(), "SwapIndexFloatLegSpec.float_tenor for id: " + sid);
        r.floatCalendar = CalendarToQL(requireEnum(
            fl->float_calendar(), "SwapIndexFloatLegSpec.float_calendar for id: " + sid));
        r.floatBdc = ConventionToQL(requireEnum(
            fl->float_bdc(), "SwapIndexFloatLegSpec.float_bdc for id: " + sid));
        r.floatTermBdc = ConventionToQL(requireEnum(
            fl->float_term_bdc(), "SwapIndexFloatLegSpec.float_term_bdc for id: " + sid));
        r.floatDateRule = DateGenerationToQL(requireEnum(
            fl->float_date_rule(), "SwapIndexFloatLegSpec.float_date_rule for id: " + sid));
        r.floatEom = requireBool(
            fl->float_eom(), "SwapIndexFloatLegSpec.float_eom for id: " + sid);

        if (r.spotDays < 0) {
            QUANTRA_INVALID_ARGUMENT("SwapIndexDef.spot_days must be >= 0 for id: " + d->id()->str());
        }

        if (r.kind == quantra::SwapIndexKind_OisSwapIndex) {
            auto on = indices.getOvernight(r.floatIndexId);
            if (on->fixingCalendar() != r.fixedCalendar || on->fixingCalendar() != r.floatCalendar) {
                QUANTRA_INVALID_ARGUMENT(
                    "OIS swap index calendars must match overnight index calendar for id: " + d->id()->str());
            }
            if (r.fixedBdc != r.floatBdc) {
                QUANTRA_INVALID_ARGUMENT(
                    "OIS swap index requires fixed_leg.fixed_bdc == float_leg.float_bdc for id: " +
                    d->id()->str());
            }
        }
        reg.add(d->id()->str(), r);
    }
    return reg;
}

} // namespace quantra
