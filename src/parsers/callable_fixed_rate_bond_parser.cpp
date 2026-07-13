#include "callable_fixed_rate_bond_parser.h"

#include <ql/instruments/bond.hpp>
#include <ql/instruments/callabilityschedule.hpp>
#include <ql/time/date.hpp>

#include "error.h"

std::shared_ptr<QuantLib::CallableFixedRateBond> CallableFixedRateBondParser::parse(
    const quantra::CallableFixedRateBond *bond)
{
    if (bond == NULL)
        QUANTRA_INVALID_ARGUMENT("CallableFixedRateBond not found");

    if (!bond->accrual_day_counter().has_value())
        QUANTRA_INVALID_ARGUMENT("CallableFixedRateBond.accrual_day_counter is required");
    if (!bond->payment_convention().has_value())
        QUANTRA_INVALID_ARGUMENT("CallableFixedRateBond.payment_convention is required");
    if (bond->schedule() == nullptr)
        QUANTRA_INVALID_ARGUMENT("CallableFixedRateBond.schedule is required");
    if (bond->issue_date() == nullptr)
        QUANTRA_INVALID_ARGUMENT("CallableFixedRateBond.issue_date is required");

    ScheduleParser schedule_parser;
    auto schedule = schedule_parser.parse(bond->schedule());
    auto dayCounter = DayCounterToQL(bond->accrual_day_counter().value());
    auto paymentConvention = ConventionToQL(bond->payment_convention().value());
    auto issueDate = DateToQL(bond->issue_date()->str());

    // The call schedule is `(required)` on the wire, but a present-yet-empty
    // vector must still be rejected: an empty call schedule is a plain
    // fixed-rate bond and would price with no optionality.
    const auto *entries = bond->call_schedule();
    if (entries == nullptr || entries->size() == 0)
        QUANTRA_INVALID_ARGUMENT("CallableFixedRateBond.call_schedule is required and must be non-empty");

    QuantLib::CallabilitySchedule callSchedule;
    callSchedule.reserve(entries->size());
    QuantLib::Date previousDate;
    for (flatbuffers::uoffset_t i = 0; i < entries->size(); ++i) {
        const auto *entry = entries->Get(i);
        if (entry == nullptr)
            QUANTRA_INVALID_ARGUMENT("CallableFixedRateBond.call_schedule contains a null entry");
        if (entry->date() == nullptr)
            QUANTRA_INVALID_ARGUMENT("CallabilityEntry.date is required");
        if (!entry->callability_type().has_value())
            QUANTRA_INVALID_ARGUMENT("CallabilityEntry.callability_type is required");
        if (!(entry->price() > 0.0))
            QUANTRA_INVALID_ARGUMENT("CallabilityEntry.price must be > 0");

        auto date = DateToQL(entry->date()->str());
        if (date <= issueDate)
            QUANTRA_INVALID_ARGUMENT("CallabilityEntry.date must be after CallableFixedRateBond.issue_date");
        if (i > 0 && date <= previousDate)
            QUANTRA_INVALID_ARGUMENT("CallableFixedRateBond.call_schedule dates must be strictly increasing");
        previousDate = date;

        QuantLib::Callability::Type callabilityType;
        switch (entry->callability_type().value()) {
            case quantra::enums::CallabilityType_Call:
                callabilityType = QuantLib::Callability::Call;
                break;
            case quantra::enums::CallabilityType_Put:
                callabilityType = QuantLib::Callability::Put;
                break;
            default:
                QUANTRA_INVALID_ARGUMENT("Invalid CallabilityEntry.callability_type");
        }

        // Clean call/put price per 100 (QuantLib Bond::Price::Clean). Dirty
        // schedules are a documented future extension (see the wire schema).
        QuantLib::Bond::Price price(entry->price(), QuantLib::Bond::Price::Clean);
        callSchedule.push_back(
            QuantLib::ext::make_shared<QuantLib::Callability>(price, callabilityType, date));
    }

    return std::make_shared<QuantLib::CallableFixedRateBond>(
        bond->settlement_days(),
        bond->face_amount(),
        *schedule,
        std::vector<QuantLib::Rate>(1, bond->rate()),
        dayCounter,
        paymentConvention,
        bond->redemption(),
        issueDate,
        callSchedule);
}
