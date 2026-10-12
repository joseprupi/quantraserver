#ifndef QUANTRA_FX_SPOT_PARSER_H
#define QUANTRA_FX_SPOT_PARSER_H

#include "fx_spot_domain.h"
#include "quote_registry.h"

#include "fx_generated.h"

namespace quantra {

/**
 * FxSpotParser - FxSpotSpec (wire) -> FxSpot (domain).
 *
 * Every field is validated here and an invalid one is a 400 naming the
 * field; a `quote_id` resolves through the QuoteRegistry and must carry the
 * Fx quote type (a Curve quote would be shifted by the rate bump). The parser
 * does not check uniqueness across spots — FxSpotRegistryBuilder does.
 */
class FxSpotParser {
public:
    FxSpot parse(const quantra::FxSpotSpec* spec, const QuoteRegistry* quotes) const;
};

/**
 * Joint settlement calendar of an FxSpotSpec: one calendar is used as is,
 * several are joined so that a holiday in any one is a holiday of the pair
 * (QuantLib JointCalendar, JoinHolidays). An empty list is a 400 naming
 * FxSpotSpec.settlement_calendars. Shared by the parser and by the
 * bootstrap-curves mapper, which reproduces the helper's settlement dates.
 */
QuantLib::Calendar JointSettlementCalendar(
    const flatbuffers::Vector<int8_t>* calendars, const std::string& spotId);

/**
 * Require a currency field to be present and a three-letter uppercase ISO
 * 4217 code (a pair string such as "EURUSD" is not a currency). `field` names
 * the wire field in the 400; `context` is appended to the message when
 * non-empty (e.g. " (spot 'EURUSD_SPOT')"). Shared by the FX spot parser and
 * the FX product mappers.
 */
std::string RequireIsoCurrency(
    const flatbuffers::String* value, const std::string& field, const std::string& context = "");

} // namespace quantra

#endif // QUANTRA_FX_SPOT_PARSER_H
