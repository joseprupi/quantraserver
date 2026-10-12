#include "fx_spot_parser.h"

#include "enum_convert.h"
#include "error.h"
#include "request_validation.h"

#include <ql/quotes/simplequote.hpp>
#include <ql/time/calendars/jointcalendar.hpp>

#include <cctype>
#include <memory>
#include <vector>

namespace quantra {

namespace {

bool isIsoCurrencyCode(const std::string& code) {
    if (code.size() != 3) return false;
    for (const char c : code) {
        if (!std::isupper(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string requireCurrency(
    const flatbuffers::String* value, const std::string& field, const std::string& spotId) {
    if (!value || value->str().empty()) {
        QUANTRA_INVALID_ARGUMENT(field + " is required (spot '" + spotId + "')");
    }
    const std::string code = value->str();
    if (!isIsoCurrencyCode(code)) {
        QUANTRA_INVALID_ARGUMENT(
            field + " must be a three-letter uppercase ISO currency code, got '" + code +
            "' (spot '" + spotId + "')");
    }
    return code;
}

} // namespace

FxSpot FxSpotParser::parse(const quantra::FxSpotSpec* spec, const QuoteRegistry* quotes) const {
    if (!spec) {
        QUANTRA_INVALID_ARGUMENT("FxSpotSpec is required");
    }
    if (!spec->id() || spec->id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("FxSpotSpec.id is required");
    }

    FxSpot spot;
    spot.id = spec->id()->str();
    spot.baseCurrency = requireCurrency(spec->base_currency(), "FxSpotSpec.base_currency", spot.id);
    spot.quoteCurrency =
        requireCurrency(spec->quote_currency(), "FxSpotSpec.quote_currency", spot.id);
    if (spot.baseCurrency == spot.quoteCurrency) {
        QUANTRA_INVALID_ARGUMENT(
            "FxSpotSpec.base_currency and FxSpotSpec.quote_currency must differ, both are '" +
            spot.baseCurrency + "' (spot '" + spot.id + "')");
    }

    // --- Rate: exactly one of the inline value or a shared quote. The inline
    // path is presence-based (a bare double would default to a silent zero).
    const bool hasRate = spec->rate().has_value();
    const bool hasQuote = spec->quote_id() && !spec->quote_id()->str().empty();
    if (hasRate == hasQuote) {
        QUANTRA_INVALID_ARGUMENT(
            "FxSpotSpec requires exactly one of rate or quote_id (spot '" + spot.id + "')");
    }
    if (hasRate) {
        const double rate = requirePositive(spec->rate(), "FxSpotSpec.rate");
        spot.rate =
            QuantLib::Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(rate));
    } else {
        if (!quotes) {
            QUANTRA_ERROR(
                "FxSpotSpec.quote_id given but no QuoteRegistry is available (spot '" + spot.id +
                "')");
        }
        const std::string quoteId = spec->quote_id()->str();
        if (!quotes->has(quoteId)) {
            QUANTRA_NOT_FOUND(
                "FxSpotSpec.quote_id: unknown quote id '" + quoteId + "' (spot '" + spot.id + "')");
        }
        try {
            spot.rate = quotes->getHandle(quoteId, quantra::QuoteType_Fx);
        } catch (const QuantraInvalidArgument&) {
            QUANTRA_INVALID_ARGUMENT(
                "FxSpotSpec.quote_id: quote '" + quoteId +
                "' must have quote_type Fx (curve quotes are shifted by the rate bump, an FX "
                "spot must not be) (spot '" +
                spot.id + "')");
        }
        if (!(spot.rate->value() > 0.0)) {
            QUANTRA_INVALID_ARGUMENT(
                "FxSpotSpec.quote_id: quote '" + quoteId + "' must be positive (spot '" + spot.id +
                "')");
        }
    }

    spot.spotDays = requireNonNegativeInt(spec->spot_days(), "FxSpotSpec.spot_days");

    spot.settlementCalendar = JointSettlementCalendar(spec->settlement_calendars(), spot.id);

    spot.businessDayConvention = ConventionToQL(
        requireEnum(spec->business_day_convention(), "FxSpotSpec.business_day_convention"));

    return spot;
}

QuantLib::Calendar JointSettlementCalendar(
    const flatbuffers::Vector<int8_t>* calendars, const std::string& spotId) {
    // At least one; joined so a holiday in any one is a holiday of the pair
    // (QuantLib JointCalendar, JoinHolidays).
    if (!calendars || calendars->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "FxSpotSpec.settlement_calendars must list at least one calendar (spot '" + spotId +
            "')");
    }
    std::vector<QuantLib::Calendar> joined;
    joined.reserve(calendars->size());
    for (flatbuffers::uoffset_t i = 0; i < calendars->size(); ++i) {
        joined.push_back(CalendarToQL(static_cast<quantra::enums::Calendar>(calendars->Get(i))));
    }
    return joined.size() == 1
               ? joined.front()
               : QuantLib::Calendar(QuantLib::JointCalendar(joined, QuantLib::JoinHolidays));
}

} // namespace quantra
