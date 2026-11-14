#include "commodity_spot_price.h"

#include "date_convert.h"
#include "error.h"

#include <cmath>
#include <string>

namespace quantra { namespace commodities {

SpotPrice::SpotPrice(
    std::string id,
    PricingCalendar pricingCalendar,
    SpotInterpolation interpolation,
    bool extrapolateFlat,
    std::vector<SpotPoint> points,
    const std::vector<SpotFixing>& fixings)
    : id_(std::move(id)), pricingCalendar_(std::move(pricingCalendar)),
      interpolation_(interpolation), extrapolateFlat_(extrapolateFlat), points_(std::move(points)) {
    const std::string where = "commodity spot price '" + id_ + "'";
    if (points_.empty()) {
        QUANTRA_INVALID_ARGUMENT(where + ": at least one point is required");
    }
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const std::string at = where + " points[" + std::to_string(i) + "]";
        if (!std::isfinite(points_[i].price)) {
            QUANTRA_INVALID_ARGUMENT(at + ": price must be finite");
        }
        if (i > 0 && points_[i].date <= points_[i - 1].date) {
            QUANTRA_INVALID_ARGUMENT(
                at + ": date " + DateToIso(points_[i].date) +
                " is not after the previous point; points must be in ascending date order");
        }
    }
    for (std::size_t i = 0; i < fixings.size(); ++i) {
        const std::string at = where + " fixings[" + std::to_string(i) + "]";
        if (!std::isfinite(fixings[i].value)) {
            QUANTRA_INVALID_ARGUMENT(at + ": value must be finite");
        }
        if (!fixings_.emplace(fixings[i].date, fixings[i].value).second) {
            QUANTRA_INVALID_ARGUMENT(at + ": a second fixing on " + DateToIso(fixings[i].date));
        }
    }
}

std::vector<SpotPointWeight> SpotPrice::weights(const QuantLib::Date& date) const {
    const std::size_t n = points_.size();
    if (date < points_.front().date || date > points_.back().date) {
        if (!extrapolateFlat_) {
            QUANTRA_INVALID_ARGUMENT(
                "commodity spot price '" + id_ + "': " + DateToIso(date) +
                " is outside its points (" + DateToIso(points_.front().date) + " to " +
                DateToIso(points_.back().date) + ") and extrapolate_flat is false");
        }
        return {{date < points_.front().date ? 0 : n - 1, 1.0}};
    }
    // First point on or after the date.
    std::size_t hi = 0;
    while (points_[hi].date < date) {
        ++hi;
    }
    if (points_[hi].date == date) {
        return {{hi, 1.0}};
    }
    const std::size_t lo = hi - 1; // date strictly between points lo and hi
    switch (interpolation_) {
        case SpotInterpolation::BackwardFlat: return {{hi, 1.0}};
        case SpotInterpolation::ForwardFlat: return {{lo, 1.0}};
        case SpotInterpolation::Linear:
        default: {
            // Actual/365 (Fixed) times: the 365 cancels, the ratio is in days.
            const double span = static_cast<double>(points_[hi].date - points_[lo].date);
            const double w = static_cast<double>(date - points_[lo].date) / span;
            return {{lo, 1.0 - w}, {hi, w}};
        }
    }
}

double SpotPrice::forward(const QuantLib::Date& date) const {
    return weightedPrice(weights(date));
}

double SpotPrice::weightedPrice(const std::vector<SpotPointWeight>& weights) const {
    double out = 0.0;
    for (const auto& w : weights) {
        out += w.weight * points_[w.point].price;
    }
    return out;
}

std::optional<double> SpotPrice::fixing(const QuantLib::Date& date) const {
    const auto it = fixings_.find(date);
    if (it == fixings_.end()) {
        return std::nullopt;
    }
    return it->second;
}

}} // namespace quantra::commodities
