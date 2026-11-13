#ifndef QUANTRA_QUOTE_WEIGHTING_H
#define QUANTRA_QUOTE_WEIGHTING_H

/**
 * Quote weights of a volatility surface fit, shared by the three methods of
 * /calibrate-vol-surface (SviPerExpiry, Ssvi, AndreasenHuge).
 *
 * A fit minimises, over the used quotes of one expiry,
 *
 *     sum_i weight_i (model_vol_i - market_vol_i)^2
 *
 * with weight_i
 *   Uniform  1;
 *   Vega     the Black vega of the quote at its market vol,
 *            D F sqrt(T) pdf(d1);
 *   BidAsk   1 / spread_i^2, spread_i the bid-ask spread in vol units (a
 *            price spread is divided by the vega). A quote given with both
 *            sides whose spread is not > 0 is left out (reason
 *            kAskNotAboveBid). A quote without both sides takes the median
 *            spread of the two-sided quotes of its expiry; an expiry without
 *            any is a 400 naming the expiry.
 *
 * Usage, per expiry: add() every candidate quote in order (false = left
 * out), then weights() or normalizedWeights() for the quotes kept.
 */

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace quantra {

enum class QuoteWeighting { Uniform, Vega, BidAsk };

/// The value's name on the wire ("Uniform", "Vega", "BidAsk").
const char* quoteWeightingName(QuoteWeighting weighting);

/// Reason reported for a quote BidAsk weighting leaves out.
inline constexpr char kAskNotAboveBid[] = "ask_not_above_bid";

/// Black vega (price per unit of vol) at the market vol: D F sqrt(T) pdf(d1).
double blackVegaAtMarketVol(
    double strike, double forward, double discount, double time, double vol);

/// Bid and ask of a price quote in vol units: the market vol minus / plus the
/// price half-spreads over the Black vega at the market vol. Empty when the
/// vega is not positive and finite (the quote then takes its expiry's median
/// spread under BidAsk).
struct VolBidAsk {
    double bid = 0.0;
    double ask = 0.0;
};
std::optional<VolBidAsk> volBidAsk(
    double strike,
    double forward,
    double discount,
    double time,
    double vol,
    double bidPrice,
    double midPrice,
    double askPrice);

class ExpiryQuoteWeights {
public:
    ExpiryQuoteWeights(QuoteWeighting weighting, double forward, double discount, double time);

    /// Adds a quote: `vol` its market vol, `bid` / `ask` its two sides when
    /// given, in vol units or (`bidAskArePrices`) in price units. Returns
    /// false, and does not add it, when BidAsk leaves the quote out
    /// (kAskNotAboveBid).
    bool add(
        double strike,
        double vol,
        const std::optional<double>& bid,
        const std::optional<double>& ask,
        bool bidAskArePrices);

    std::size_t size() const { return vega_.size(); }

    /// The weights of the quotes added, in order. BidAsk: 400 naming
    /// `expiryIso` when no quote added has both sides.
    std::vector<double> weights(const std::string& expiryIso) const;

    /// The same weights scaled to sum 1 (a fit that adds penalty terms to
    /// the residuals keeps the same balance whatever the weighting). Under
    /// Vega the factors common to the expiry (D sqrt(T)) are left out
    /// before the scaling instead of cancelled by it, which keeps the
    /// SviPerExpiry weights of earlier versions to the last bit.
    std::vector<double> normalizedWeights(const std::string& expiryIso) const;

private:
    QuoteWeighting weighting_;
    double forward_, discount_, time_;
    std::vector<double> vega_;      // D F sqrt(T) pdf(d1)
    std::vector<double> unitVega_;  // F pdf(d1)
    std::vector<double> volSpread_; // NaN when the quote lacks a side
};

} // namespace quantra

#endif // QUANTRA_QUOTE_WEIGHTING_H
