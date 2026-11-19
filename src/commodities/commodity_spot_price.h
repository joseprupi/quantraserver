#ifndef QUANTRA_COMMODITIES_SPOT_PRICE_H
#define QUANTRA_COMMODITIES_SPOT_PRICE_H

/**
 * A spot price assessment (a published daily price) with forward prices by
 * date and past published prices.
 *
 * The forward price on a date is interpolated between the dated points in
 * Actual/365 (Fixed) time, i.e. in calendar days:
 *   Linear        straight line between the two surrounding points;
 *   BackwardFlat  the price of the next point (on a point: that point);
 *   ForwardFlat   the price of the previous point (on a point: that point).
 * A date before the first point or after the last one is refused unless
 * `extrapolateFlat`, which holds the first / last price flat.
 */

#include "commodity_pricing_days.h"
#include "error.h"

#include <ql/time/date.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace quantra { namespace commodities {

enum class SpotInterpolation { Linear, BackwardFlat, ForwardFlat };

struct SpotPoint {
    QuantLib::Date date;
    double price = 0.0;
};

struct SpotFixing {
    QuantLib::Date date;
    double value = 0.0;
};

/// Weight of one dated point in an interpolated price.
struct SpotPointWeight {
    /// Index into SpotPrice::points().
    std::size_t point = 0;
    double weight = 0.0;
};

/// A date read outside the points while extrapolate_flat is false: the
/// invalid argument SpotPrice::weights raises, with the date as data.
class SpotDateOutsidePoints : public QuantraInvalidArgument {
public:
    SpotDateOutsidePoints(const std::string& message, const QuantLib::Date& date)
        : QuantraInvalidArgument(message), date_(date) {}
    const QuantLib::Date& date() const { return date_; }

private:
    QuantLib::Date date_;
};

class SpotPrice {
public:
    /// Validates the values: at least one point; point dates strictly
    /// ascending; prices and fixing values finite; no fixing date given twice.
    /// Presence (id, dates) is the caller's: the request decoder checks it.
    SpotPrice(
        std::string id,
        PricingCalendar pricingCalendar,
        SpotInterpolation interpolation,
        bool extrapolateFlat,
        std::vector<SpotPoint> points,
        const std::vector<SpotFixing>& fixings);

    const std::string& id() const { return id_; }
    const PricingCalendar& pricingCalendar() const { return pricingCalendar_; }
    const std::vector<SpotPoint>& points() const { return points_; }
    SpotInterpolation interpolation() const { return interpolation_; }
    bool extrapolateFlat() const { return extrapolateFlat_; }

    /// Interpolated forward price on `date`: weightedPrice(weights(date)).
    double forward(const QuantLib::Date& date) const;

    /// The points the forward price on `date` is made of and their weights;
    /// weights that are 0 are left out. SpotDateOutsidePoints outside the
    /// points unless extrapolating.
    std::vector<SpotPointWeight> weights(const QuantLib::Date& date) const;

    /// Sum of weight * point price over `weights` (from weights()).
    double weightedPrice(const std::vector<SpotPointWeight>& weights) const;

    /// The published price on `date`, when given.
    std::optional<double> fixing(const QuantLib::Date& date) const;

private:
    std::string id_;
    PricingCalendar pricingCalendar_;
    SpotInterpolation interpolation_;
    bool extrapolateFlat_;
    std::vector<SpotPoint> points_;
    std::map<QuantLib::Date, double> fixings_;
};

}} // namespace quantra::commodities

#endif // QUANTRA_COMMODITIES_SPOT_PRICE_H
