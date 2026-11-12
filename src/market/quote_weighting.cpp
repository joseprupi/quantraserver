#include "quote_weighting.h"

#include "error.h"

#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/pricingengines/blackformula.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace quantra {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

} // namespace

const char* quoteWeightingName(QuoteWeighting weighting) {
    switch (weighting) {
        case QuoteWeighting::Uniform: return "Uniform";
        case QuoteWeighting::Vega: return "Vega";
        case QuoteWeighting::BidAsk: return "BidAsk";
    }
    return "?";
}

double blackVegaAtMarketVol(
    double strike, double forward, double discount, double time, double vol) {
    const double sd = vol * std::sqrt(time);
    const double d1 = std::log(forward / strike) / sd + 0.5 * sd;
    return discount * forward * std::sqrt(time) *
           QuantLib::CumulativeNormalDistribution().derivative(d1);
}

std::optional<VolBidAsk> volBidAsk(
    double strike,
    double forward,
    double discount,
    double time,
    double vol,
    double bidPrice,
    double midPrice,
    double askPrice) {
    const double sqrtT = std::sqrt(time);
    const double vega =
        QuantLib::blackFormulaStdDevDerivative(strike, forward, vol * sqrtT, discount) * sqrtT;
    if (!(vega > 0.0 && std::isfinite(vega))) return std::nullopt;
    return VolBidAsk{vol - (midPrice - bidPrice) / vega, vol + (askPrice - midPrice) / vega};
}

ExpiryQuoteWeights::ExpiryQuoteWeights(
    QuoteWeighting weighting, double forward, double discount, double time)
    : weighting_(weighting), forward_(forward), discount_(discount), time_(time) {}

bool ExpiryQuoteWeights::add(
    double strike,
    double vol,
    const std::optional<double>& bid,
    const std::optional<double>& ask,
    bool bidAskArePrices) {
    const double vega = blackVegaAtMarketVol(strike, forward_, discount_, time_, vol);
    double spread = kNaN;
    if (weighting_ == QuoteWeighting::BidAsk && bid && ask) {
        spread = *ask - *bid;
        if (bidAskArePrices) spread /= std::max(vega, 1e-300);
        if (!(spread > 0.0)) return false;
    }
    vega_.push_back(vega);
    unitVega_.push_back(
        weighting_ == QuoteWeighting::Vega
            ? QuantLib::blackFormulaStdDevDerivative(strike, forward_, vol * std::sqrt(time_), 1.0)
            : kNaN);
    volSpread_.push_back(spread);
    return true;
}

std::vector<double> ExpiryQuoteWeights::weights(const std::string& expiryIso) const {
    std::vector<double> w(vega_.size(), 1.0);
    if (weighting_ == QuoteWeighting::Vega) {
        w = vega_;
    } else if (weighting_ == QuoteWeighting::BidAsk) {
        std::vector<double> known;
        for (double s : volSpread_) {
            if (!std::isnan(s)) known.push_back(s);
        }
        if (known.empty()) {
            QUANTRA_INVALID_ARGUMENT(
                "weighting BidAsk needs bid and ask on at least one used quote at expiry " +
                expiryIso);
        }
        const double med = median(known);
        for (std::size_t i = 0; i < w.size(); ++i) {
            const double s = std::isnan(volSpread_[i]) ? med : volSpread_[i];
            w[i] = 1.0 / (s * s);
        }
    }
    return w;
}

std::vector<double> ExpiryQuoteWeights::normalizedWeights(const std::string& expiryIso) const {
    std::vector<double> w = weighting_ == QuoteWeighting::Vega ? unitVega_ : weights(expiryIso);
    double sum = 0.0;
    for (double x : w)
        sum += x;
    for (double& x : w)
        x /= sum;
    return w;
}

} // namespace quantra
