#include "commodity_spot_price.h"

#include "date_convert.h"
#include "error.h"

#include <cmath>
#include <string>

namespace quantra { namespace commodities {

CommodityReadData SpotDateOutsidePoints::dataFor(
    const QuantLib::Date& date, const std::string& spotId, const std::string& path) {
    CommodityReadData data;
    data.reason = "DateOutsidePoints";
    data.path = path;
    data.date = DateToIso(date);
    data.spotId = spotId;
    return data;
}

SpotPrice::SpotPrice(
    std::string id,
    PricingCalendar pricingCalendar,
    SpotInterpolation interpolation,
    bool extrapolateFlat,
    std::vector<SpotPoint> points,
    const std::vector<SpotFixing>& fixings)
    : id_(std::move(id)), pricingCalendar_(std::move(pricingCalendar)),
      interpolation_(interpolation), extrapolateFlat_(extrapolateFlat), points_(std::move(points)) {
    // Messages start with the field, relative to the spot price (the caller
    // puts its path in front): `points[2].date ...`.
    if (points_.empty()) {
        QuantraInvalidArgumentAt("points", "points: at least one point is required");
    }
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const std::string field = "points[" + std::to_string(i) + "]";
        if (!std::isfinite(points_[i].price)) {
            QuantraInvalidArgumentAt(field + ".price", field + ".price must be finite");
        }
        if (i > 0 && points_[i].date <= points_[i - 1].date) {
            QuantraInvalidArgumentAt(
                field + ".date",
                field + ".date " + DateToIso(points_[i].date) +
                    " is not after the previous point; points must be in ascending date order");
        }
    }
    for (std::size_t i = 0; i < fixings.size(); ++i) {
        const std::string field = "fixings[" + std::to_string(i) + "]";
        if (!std::isfinite(fixings[i].value)) {
            QuantraInvalidArgumentAt(field + ".value", field + ".value must be finite");
        }
        if (!fixings_.emplace(fixings[i].date, fixings[i].value).second) {
            QuantraInvalidArgumentAt(
                field, field + ": a second fixing on " + DateToIso(fixings[i].date));
        }
    }
}

std::vector<SpotPointWeight> SpotPrice::weights(const QuantLib::Date& date) const {
    const std::size_t n = points_.size();
    if (date < points_.front().date || date > points_.back().date) {
        if (!extrapolateFlat_) {
            throw SpotDateOutsidePoints(
                "commodity spot price '" + id_ + "': " + DateToIso(date) +
                    " is outside its points (" + DateToIso(points_.front().date) + " to " +
                    DateToIso(points_.back().date) + ") and extrapolate_flat is false",
                date, id_);
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
