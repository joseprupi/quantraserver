#ifndef QUANTRA_FX_SPOT_DOMAIN_H
#define QUANTRA_FX_SPOT_DOMAIN_H

/**
 * Plain-domain mirror of flatbuffers/fbs/fx.fbs:FxSpotSpec.
 *
 * Produced by FxSpotParser (parsers layer) from the wire table and stored in
 * FxSpotRegistry (market layer); consumed by the FX curve helpers and, later,
 * by the FX product evaluators. Holds QuantLib values only — no FlatBuffers
 * pointers — so evaluators stay FlatBuffers-free.
 */

#include <ql/handle.hpp>
#include <ql/quote.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>

#include <string>

namespace quantra {

struct FxSpot {
    /// Identifier referenced by helpers and products.
    std::string id;
    /// ISO code of the base currency: one unit of it costs `rate` units of
    /// the quote currency.
    std::string baseCurrency;
    /// ISO code of the quote currency.
    std::string quoteCurrency;
    /// Spot rate for delivery on the spot date (quote units per base unit).
    /// A handle so a shared quote (quote_id) is observed, never copied.
    QuantLib::Handle<QuantLib::Quote> rate;
    /// Spot lag in business days on the settlement calendar.
    int spotDays = 0;
    /// Joint calendar of every settlement calendar listed on the spec.
    QuantLib::Calendar settlementCalendar;
    /// Adjustment applied to settlement dates that fall on a holiday.
    QuantLib::BusinessDayConvention businessDayConvention = QuantLib::Following;
};

} // namespace quantra

#endif // QUANTRA_FX_SPOT_DOMAIN_H
