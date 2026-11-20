#include "commodity_curve_build_evaluator.h"

#include "curve_cache.h"
#include "date_convert.h"
#include "error.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

namespace quantra {

namespace {

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

std::string jsonNumber(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

} // namespace

std::string CurveBuildRefusalJson(
    const commodities::CurveBuildRefusal& r,
    std::optional<std::size_t> curve,
    const std::string& pathPrefix) {
    std::string out =
        "{\"reason\": " + jsonString(commodities::CurveBuildRefusalReasonName(r.reason));
    if (curve) {
        out += ", \"curve\": " + std::to_string(*curve);
    }
    if (!r.path.empty()) {
        out += ", \"path\": " + jsonString(pathPrefix + r.path);
    }
    if (r.averageQuote) {
        out += ", \"average_quote\": " + std::to_string(*r.averageQuote);
    }
    if (r.month) {
        out += ", \"month\": " + jsonString(*r.month);
    }
    if (!r.months.empty()) {
        out += ", \"months\": [";
        for (std::size_t k = 0; k < r.months.size(); ++k) {
            out += (k == 0 ? "" : ", ") + jsonString(r.months[k]);
        }
        out += "]";
    }
    if (r.impliedValue) {
        out += ", \"implied_value\": " + jsonNumber(*r.impliedValue);
    }
    if (r.quotedValue) {
        out += ", \"quoted_value\": " + jsonNumber(*r.quotedValue);
    }
    if (r.gap) {
        out += ", \"gap\": " + jsonNumber(*r.gap);
    }
    if (!r.dependsOn.empty()) {
        out += ", \"depends_on\": [";
        for (std::size_t k = 0; k < r.dependsOn.size(); ++k) {
            const auto& input = r.dependsOn[k];
            out += k == 0 ? "" : ", ";
            if (input.averageQuote) {
                out += "{\"average_quote\": " + std::to_string(*input.averageQuote) + "}";
            } else if (input.date) {
                out += "{\"date\": " + jsonString(*input.date) + "}";
            } else {
                out += "{\"month\": " + jsonString(input.month.value_or("")) + "}";
            }
        }
        out += "]";
    }
    if (r.rule) {
        out += ", \"rule\": " + jsonString(commodities::CurveRuleName(*r.rule));
    }
    if (r.date) {
        out += ", \"date\": " + jsonString(*r.date);
    }
    if (!r.dates.empty()) {
        out += ", \"dates\": [";
        for (std::size_t k = 0; k < r.dates.size(); ++k) {
            out += (k == 0 ? "" : ", ") + jsonString(r.dates[k]);
        }
        out += "]";
    }
    if (r.baseCurve) {
        out += ", \"base_curve\": " + std::to_string(*r.baseCurve);
    }
    if (r.baseMonth) {
        out += ", \"base_month\": " + jsonString(*r.baseMonth);
    }
    if (!r.cycle.empty()) {
        out += ", \"cycle\": [";
        for (std::size_t k = 0; k < r.cycle.size(); ++k) {
            out += (k == 0 ? "" : ", ") + std::to_string(r.cycle[k]);
        }
        out += "]";
    }
    return out + "}";
}

namespace {

namespace cm = commodities;

std::string entryPath(const CommodityCurveSetOptions& options, std::size_t i) {
    return options.listPath + "[" + std::to_string(i) + "]";
}

[[noreturn]] void basisRefusal(
    const CommodityCurveSetOptions& options,
    cm::CurveBuildRefusal::Reason reason,
    std::size_t curve,
    const std::string& message,
    std::optional<std::size_t> baseCurve,
    const std::vector<std::size_t>& cycle) {
    cm::CurveBuildRefusal r;
    r.reason = reason;
    r.path = "basis.base";
    r.baseCurve = baseCurve;
    r.cycle = cycle;
    const std::string path = entryPath(options, curve);
    const std::string text = path + ".basis.base: " + message;
    if (reason == cm::CurveBuildRefusal::Reason::UnknownBase) {
        throw QuantraNotFound(text, CurveBuildRefusalJson(r, curve, path + "."));
    }
    throw QuantraInvalidArgument(text, CurveBuildRefusalJson(r, curve, path + "."));
}

/// The order the entries are built in: every base before the entries that
/// read it, otherwise request order. Resolves each basis's base index (an
/// entry of the same kind: a contract for a contract, a spot price for a
/// spot price).
std::vector<std::size_t> buildOrder(
    const std::vector<CommodityCurveBuildSpec>& builds,
    const CommodityCurveSetOptions& options,
    std::vector<std::optional<std::size_t>>& baseOf) {
    using Reason = cm::CurveBuildRefusal::Reason;
    const std::size_t n = builds.size();
    baseOf.assign(n, std::nullopt);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& build = builds[i];
        if (!build.basis) {
            continue;
        }
        const bool spot = build.spot.has_value();
        std::vector<std::size_t> named;
        bool otherKind = false;
        for (std::size_t j = 0; j < n; ++j) {
            const auto& other = builds[j];
            const bool contractNamed = other.contract && other.contract->id == build.baseId;
            const bool spotNamed = other.spot && other.spot->id == build.baseId;
            if (spot ? spotNamed : contractNamed) {
                named.push_back(j);
            } else if (contractNamed || spotNamed) {
                otherKind = true;
            }
        }
        const std::string kind = spot ? "spot price" : "contract";
        if (named.empty() && otherKind) {
            basisRefusal(
                options, spot ? Reason::BaseNotSpot : Reason::BaseNotContract, i,
                "'" + build.baseId + "' is a " + (spot ? "contract" : "spot price") +
                    " build; the base of a " + kind + " must be a " + kind,
                std::nullopt, {});
        }
        if (named.empty()) {
            basisRefusal(
                options, Reason::UnknownBase, i,
                "no " + kind + " build in " + options.listPath + " has id '" + build.baseId + "'",
                std::nullopt, {});
        }
        if (named.size() > 1) {
            basisRefusal(
                options, spot ? Reason::BaseNotSpot : Reason::BaseNotContract, i,
                "'" + build.baseId + "' names " + std::to_string(named.size()) + " " + kind +
                    " builds; ids of bases must be unique",
                std::nullopt, {});
        }
        if (named[0] == i) {
            const std::vector<std::size_t> self{i};
            basisRefusal(
                options, Reason::BasisCycle, i, "a " + kind + " cannot be its own base", i, self);
        }
        baseOf[i] = named[0];
    }
    std::vector<int> state(n, 0); // 0 new, 1 on the stack, 2 done
    std::vector<std::size_t> order;
    std::vector<std::size_t> stack;
    std::function<void(std::size_t)> visit = [&](std::size_t i) {
        if (state[i] == 2) {
            return;
        }
        if (state[i] == 1) {
            auto it = std::find(stack.begin(), stack.end(), i);
            std::vector<std::size_t> cycle(it, stack.end());
            std::string names;
            for (const std::size_t c : cycle) {
                names += entryPath(options, c) + " -> ";
            }
            names += entryPath(options, i);
            basisRefusal(
                options, Reason::BasisCycle, cycle.front(), "the bases form a cycle: " + names,
                baseOf[cycle.front()], cycle);
        }
        state[i] = 1;
        stack.push_back(i);
        if (baseOf[i]) {
            visit(*baseOf[i]);
        }
        stack.pop_back();
        state[i] = 2;
        order.push_back(i);
    };
    for (std::size_t i = 0; i < n; ++i) {
        visit(i);
    }
    return order;
}

/// Build one entry; `base` is its built base (basis entries only).
BuiltCommodityEntry buildOne(
    const CommodityCurveBuildSpec& build,
    std::size_t i,
    const BuiltCommodityEntry* base,
    std::optional<std::size_t> baseIndex,
    const QuantLib::Date& asOf,
    const RatesRegistry& rates,
    const CommodityCurveSetOptions& options,
    bool details,
    bool jacobian) {
    const std::string path = entryPath(options, i);

    // Ids first: an unknown discounting curve is a 404.
    cm::CurveBuildInputs core;
    core.interpolation = build.interpolation;
    core.extrapolateFlat = build.extrapolateFlat;
    core.rule = build.rule;
    core.includeDetails = details;
    core.fullJacobian = jacobian;
    core.averageQuotes = build.averageQuotes;
    if (build.basis) {
        core.basis = build.basis;
        core.basis->baseCurve = *baseIndex;
        core.basis->base = base->contract;
        core.basis->baseSpot = base->spot;
    }
    for (std::size_t k = 0; k < core.averageQuotes.size(); ++k) {
        const auto& id = build.discountingCurveIds[k];
        if (!id) {
            continue;
        }
        auto curve = rates.curves.find(*id);
        if (curve == rates.curves.end() || !curve->second) {
            cm::CurveBuildRefusal r;
            r.reason = cm::CurveBuildRefusal::Reason::UnknownDiscountingCurve;
            r.path = "average_quotes[" + std::to_string(k) + "].discounting_curve";
            r.averageQuote = k;
            throw QuantraNotFound(
                path + ".average_quotes[" + std::to_string(k) +
                    "].discounting_curve: discounting curve not found: " + *id,
                CurveBuildRefusalJson(r, i, path + "."));
        }
        core.averageQuotes[k].discountCurve =
            QuantLib::Handle<QuantLib::YieldTermStructure>(*curve->second);
    }

    BuiltCommodityEntry out;
    auto& row = out.row;
    auto rethrow = [&](const QuantraInvalidArgument& e) {
        const std::string message = cm::QualifyCurveBuildMessage(path, e.what());
        if (const auto* refused = dynamic_cast<const cm::CurveBuildError*>(&e)) {
            // A refusal of the solve or the fill: the same message, plus its
            // data as the error's details.
            throw QuantraInvalidArgument(
                message, CurveBuildRefusalJson(refused->refusal(), i, path + "."));
        }
        if (e.path().empty()) {
            QUANTRA_INVALID_ARGUMENT(message);
        }
        cm::CurveBuildRefusal r;
        r.reason = cm::CurveBuildRefusal::Reason::InvalidInput;
        r.path = e.path();
        throw QuantraInvalidArgument(message, CurveBuildRefusalJson(r, i, path + "."));
    };
    if (build.spot) {
        const auto& sdata = *build.spot;
        cm::SpotCurveSpec spec{
            sdata.id,     sdata.calendar, sdata.interpolation, sdata.extrapolateFlat,
            sdata.points, sdata.fixings};
        try {
            auto built = cm::BuildSpotCurve(spec, core, asOf);
            row.points = std::move(built.points);
            row.report = std::move(built.report);
            out.jacobian = std::move(built.jacobian);
        } catch (const QuantraInvalidArgument& e) {
            rethrow(e);
        }
        row.isSpot = true;
        row.id = sdata.id;
        row.pricingCalendarWire = build.pricingCalendarWire;
        row.pricingHolidays = build.pricingHolidays;
        row.fixingsGiven = build.fixingsGiven;
        row.spotFixings = sdata.fixings;
        row.spotInterpolationWire = build.spotInterpolationWire;
        row.extrapolateFlat = sdata.extrapolateFlat;
        std::vector<cm::SpotPoint> points;
        for (const auto& p : row.points) {
            points.push_back({p.date, *p.price});
        }
        out.spot = std::make_shared<const cm::SpotPrice>(
            sdata.id, sdata.calendar, sdata.interpolation, sdata.extrapolateFlat, points,
            sdata.fixings);
        return out;
    }
    const auto& data = *build.contract;

    // The contract's value rules (months, last trading dates, prices,
    // fixings) are its constructor's; its messages name the contract and the
    // month or fixing index.
    std::optional<cm::FuturesContract> contract;
    try {
        contract.emplace(data.id, data.calendar, data.months, data.fixings);
    } catch (const QuantraInvalidArgument& e) {
        if (e.path().empty()) {
            QUANTRA_INVALID_ARGUMENT(path + ".contract: " + e.what());
        }
        // The constructor's message starts with the field, relative to the
        // contract.
        cm::CurveBuildRefusal r;
        r.reason = cm::CurveBuildRefusal::Reason::InvalidInput;
        r.path = e.path();
        throw QuantraInvalidArgument(
            path + ".contract." + e.what(), CurveBuildRefusalJson(r, i, path + ".contract."));
    }
    try {
        auto built = cm::BuildCommodityCurve(*contract, core, asOf);
        row.months = std::move(built.months);
        row.report = std::move(built.report);
        out.jacobian = std::move(built.jacobian);
    } catch (const QuantraInvalidArgument& e) {
        rethrow(e);
    }
    row.id = data.id;
    row.pricingCalendarWire = build.pricingCalendarWire;
    row.pricingHolidays = build.pricingHolidays;
    row.fixingsGiven = build.fixingsGiven;
    row.fixings = data.fixings;
    out.contract = std::make_shared<const cm::FuturesContract>(
        data.id, data.calendar, row.months, data.fixings);
    return out;
}

/// The entry's outputs: live months (report order) or points.
std::vector<double> outputsOf(const BuiltCommodityEntry& e) {
    std::vector<double> out;
    if (e.row.isSpot) {
        for (const auto& p : e.row.report.points) {
            out.push_back(p.price);
        }
    } else {
        for (const auto& m : e.row.report.months) {
            out.push_back(m.price);
        }
    }
    return out;
}

/// The spec with one input moved by `h`: a direct price (a month or a point,
/// or its basis quote) or an average quote.
CommodityCurveBuildSpec bumped(
    const CommodityCurveBuildSpec& spec, const cm::QuoteSensitivity& input, double h) {
    CommodityCurveBuildSpec out = spec;
    if (input.averageQuote) {
        out.averageQuotes[*input.averageQuote].price += h;
        return out;
    }
    if (out.basis) {
        for (auto& q : out.basis->quotes) {
            const bool match = input.month ? q.month == *input.month
                                           : (q.date && DateToIso(*q.date) == *input.date);
            if (match) {
                q.value += h;
                return out;
            }
        }
    }
    if (input.month) {
        for (auto& m : out.contract->months) {
            if (m.month == *input.month && m.price) {
                *m.price += h;
            }
        }
    } else {
        for (auto& p : out.spot->points) {
            if (DateToIso(p.date) == *input.date && p.price) {
                *p.price += h;
            }
        }
    }
    return out;
}

/// The value of an input in the spec (for the step size).
double inputValue(const CommodityCurveBuildSpec& spec, const cm::QuoteSensitivity& input) {
    if (input.averageQuote) {
        return spec.averageQuotes[*input.averageQuote].price;
    }
    if (spec.basis) {
        for (const auto& q : spec.basis->quotes) {
            if (input.month ? q.month == *input.month
                            : (q.date && DateToIso(*q.date) == *input.date)) {
                return q.value;
            }
        }
    }
    if (input.month) {
        for (const auto& m : spec.contract->months) {
            if (m.month == *input.month && m.price) {
                return *m.price;
            }
        }
    } else {
        for (const auto& p : spec.spot->points) {
            if (DateToIso(p.date) == *input.date && p.price) {
                return *p.price;
            }
        }
    }
    return 0.0;
}

// ---------------------------------------------------------------------------
// Cache of built entries
// ---------------------------------------------------------------------------

std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

std::string optionalNumber(const std::optional<double>& v) {
    return v ? number(*v) : "-";
}

std::string optionalInt(const std::optional<int>& v) {
    return v ? std::to_string(*v) : "-";
}

/// Everything the entry's build reads, as text; empty when it reads a
/// discount curve without a cache key (then it is never cached).
std::string entryKeyText(
    const CommodityCurveBuildSpec& b, const RatesRegistry& rates, const std::string& baseKey) {
    std::ostringstream os;
    auto calendar = [&](const cm::PricingCalendar& c) {
        os << "|cal:" << c.calendar().name() << ':' << static_cast<int>(b.pricingCalendarWire);
        if (b.pricingHolidays) {
            for (const auto& h : *b.pricingHolidays) {
                os << ',' << h;
            }
        }
    };
    if (b.contract) {
        const auto& c = *b.contract;
        os << "contract:" << c.id;
        calendar(c.calendar);
        for (const auto& m : c.months) {
            os << "|m:" << m.month << ':' << DateToIso(m.lastTradeDate) << ':'
               << optionalNumber(m.price);
        }
        for (const auto& f : c.fixings) {
            os << "|f:" << f.month << ':' << DateToIso(f.date) << ':' << number(f.value);
        }
    } else {
        const auto& s = *b.spot;
        os << "spot:" << s.id << ':' << static_cast<int>(s.interpolation) << ':'
           << s.extrapolateFlat;
        calendar(s.calendar);
        for (const auto& p : s.points) {
            os << "|p:" << DateToIso(p.date) << ':' << optionalNumber(p.price);
        }
        for (const auto& f : s.fixings) {
            os << "|f:" << DateToIso(f.date) << ':' << number(f.value);
        }
    }
    os << "|interp:" << (b.interpolation ? static_cast<int>(*b.interpolation) : -1)
       << "|xf:" << b.extrapolateFlat << "|rule:" << (b.rule ? static_cast<int>(*b.rule) : -1);
    for (std::size_t k = 0; k < b.averageQuotes.size(); ++k) {
        const auto& q = b.averageQuotes[k];
        os << "|q:" << number(q.price) << ':' << q.nearby << ':' << q.rollDaysBeforeExpiry << ':'
           << (q.quantityBasis ? static_cast<int>(*q.quantityBasis) : -1) << ':'
           << (q.paymentCalendar ? q.paymentCalendar->name() : "-") << ':'
           << optionalInt(q.paymentLag) << ':'
           << (q.paymentConvention ? static_cast<int>(*q.paymentConvention) : -1) << ':'
           << q.versusBase << ':' << optionalInt(q.baseNearby) << ':'
           << optionalInt(q.baseRollDaysBeforeExpiry);
        for (const auto& p : q.periods) {
            os << ':' << DateToIso(p.start) << '/' << DateToIso(p.end);
        }
        if (const auto& id = b.discountingCurveIds[k]) {
            const auto key = rates.curveKeys.find(*id);
            if (key == rates.curveKeys.end()) {
                return "";
            }
            os << ":dc=" << key->second;
        }
    }
    if (b.basis) {
        const auto& x = *b.basis;
        os << "|basis:" << static_cast<int>(x.basePrice) << ':' << x.baseMonthOffset << ':'
           << optionalInt(x.baseNearby) << ':' << optionalInt(x.baseRollDaysBeforeExpiry)
           << ":base=" << baseKey;
        for (const auto& q : x.quotes) {
            os << ":" << q.month << '/' << (q.date ? DateToIso(*q.date) : "-") << '/'
               << number(q.value);
        }
    }
    return os.str();
}

class BuiltEntryCache {
public:
    static BuiltEntryCache& instance() {
        static BuiltEntryCache cache;
        return cache;
    }
    std::shared_ptr<const BuiltCommodityEntry> get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            return nullptr;
        }
        lru_.splice(lru_.begin(), lru_, it->second.second);
        return it->second.first;
    }
    void put(const std::string& key, std::shared_ptr<const BuiltCommodityEntry> value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (map_.count(key) > 0) {
            return;
        }
        while (map_.size() >= 100 && !lru_.empty()) {
            map_.erase(lru_.back());
            lru_.pop_back();
        }
        lru_.push_front(key);
        map_[key] = {std::move(value), lru_.begin()};
    }

private:
    std::mutex mutex_;
    std::list<std::string> lru_;
    std::unordered_map<
        std::string,
        std::pair<std::shared_ptr<const BuiltCommodityEntry>, std::list<std::string>::iterator>>
        map_;
};

std::string hexDigest(const std::string& text) {
    // FNV-1a 64-bit, twice with different offsets: a short, stable label for
    // logs; the cache map is keyed on the full text.
    auto fnv = [&](std::uint64_t h) {
        for (const unsigned char c : text) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    };
    char buf[40];
    std::snprintf(
        buf, sizeof(buf), "cb:%016llx%016llx",
        static_cast<unsigned long long>(fnv(1469598103934665603ULL)),
        static_cast<unsigned long long>(fnv(1099511628211ULL)));
    return buf;
}

} // namespace

std::vector<BuiltCommodityEntry> BuildCommodityCurveSet(
    const std::vector<CommodityCurveBuildSpec>& builds,
    const QuantLib::Date& asOf,
    const RatesRegistry& rates,
    const RequestBudget* budget,
    const CommodityCurveSetOptions& options) {
    std::vector<std::optional<std::size_t>> baseIndex;
    const auto order = buildOrder(builds, options, baseIndex);
    const std::size_t n = builds.size();
    // An entry that is someone's base needs its own jacobian inputs listed
    // (for the chain) when details or jacobians are asked for.
    std::vector<bool> isBase(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        if (baseIndex[i]) {
            isBase[*baseIndex[i]] = true;
        }
    }
    std::vector<std::optional<BuiltCommodityEntry>> built(n);
    std::vector<std::string> keys(n);
    auto& cacheLog = CurveCache::instance();
    for (const std::size_t i : order) {
        if (budget != nullptr) {
            budget->check();
        }
        const auto& build = builds[i];
        const bool wantJacobian = options.jacobians || (options.includeDetails && isBase[i]) ||
                                  (options.includeDetails && build.basis.has_value());
        const BuiltCommodityEntry* base = baseIndex[i] ? &*built[*baseIndex[i]] : nullptr;

        std::string key;
        if (options.cache) {
            const std::string baseKey = baseIndex[i] ? keys[*baseIndex[i]] : "";
            const std::string text = (baseIndex[i] && baseKey.empty())
                                         ? std::string()
                                         : entryKeyText(build, rates, baseKey);
            if (!text.empty()) {
                key = "v1|" + DateToIso(asOf) + "|" + options.requestKey + "|" +
                      std::to_string(wantJacobian) + std::to_string(options.includeDetails) + "|" +
                      text;
            }
        }
        keys[i] = key;
        if (!key.empty()) {
            if (auto hit = BuiltEntryCache::instance().get(key)) {
                cacheLog.stats().l1_hits++;
                cacheLog.logEvent(
                    build.contract ? build.contract->id : build.spot->id, hexDigest(key), "L1_HIT");
                built[i] = *hit;
                continue;
            }
            cacheLog.stats().l1_misses++;
            cacheLog.logEvent(
                build.contract ? build.contract->id : build.spot->id, hexDigest(key), "L1_MISS");
        }

        auto entry = buildOne(
            build, i, base, baseIndex[i], asOf, rates, options, options.includeDetails,
            wantJacobian);

        // The bases' inputs: central differences of the chain, rebuilt with
        // one input of one base moved.
        if (wantJacobian && build.basis) {
            std::vector<std::size_t> chain; // base, base of base, ...
            for (auto j = baseIndex[i]; j; j = baseIndex[*j]) {
                chain.push_back(*j);
            }
            const std::size_t rowsCount = entry.jacobian.rows.size();
            for (std::size_t c = 0; c < chain.size(); ++c) {
                const std::size_t j = chain[c];
                for (const auto& input : built[j]->jacobian.inputs) {
                    if (input.curve) {
                        continue; // an input of j's own base: listed under that base
                    }
                    const double h = 1e-6 * std::max(1.0, std::fabs(inputValue(builds[j], input)));
                    std::vector<double> sides[2];
                    for (int s = 0; s < 2; ++s) {
                        const double step = s == 0 ? h : -h;
                        // Rebuild j with the input moved, then each entry down
                        // the chain to i.
                        std::optional<BuiltCommodityEntry> prev = buildOne(
                            bumped(builds[j], input, step), j,
                            baseIndex[j] ? &*built[*baseIndex[j]] : nullptr, baseIndex[j], asOf,
                            rates, options, false, false);
                        for (std::size_t d = c; d-- > 0;) {
                            const std::size_t k = chain[d];
                            prev = buildOne(
                                builds[k], k, &*prev, baseIndex[k], asOf, rates, options, false,
                                false);
                        }
                        sides[s] = outputsOf(buildOne(
                            build, i, &*prev, baseIndex[i], asOf, rates, options, false, false));
                    }
                    cm::QuoteSensitivity label = input;
                    label.curve = j;
                    entry.jacobian.inputs.push_back(label);
                    for (std::size_t r = 0; r < rowsCount; ++r) {
                        entry.jacobian.rows[r].push_back((sides[0][r] - sides[1][r]) / (2.0 * h));
                    }
                }
            }
            if (options.includeDetails) {
                // Each determined output also lists the bases' inputs.
                const std::size_t own = entry.jacobian.inputs.size();
                auto& report = entry.row.report;
                auto add = [&](std::vector<cm::QuoteSensitivity>& list, std::size_t r) {
                    if (list.empty()) {
                        return;
                    }
                    for (std::size_t e = 0; e < own; ++e) {
                        const auto& input = entry.jacobian.inputs[e];
                        if (!input.curve) {
                            continue;
                        }
                        cm::QuoteSensitivity qs = input;
                        qs.value = entry.jacobian.rows[r][e];
                        list.push_back(qs);
                        report.maxSensitivity =
                            std::max(report.maxSensitivity.value_or(0.0), std::fabs(qs.value));
                    }
                };
                for (std::size_t r = 0; r < report.months.size(); ++r) {
                    add(report.months[r].sensitivities, r);
                }
                for (std::size_t r = 0; r < report.points.size(); ++r) {
                    add(report.points[r].sensitivities, r);
                }
            }
        }
        if (!key.empty()) {
            BuiltEntryCache::instance().put(
                key, std::make_shared<const BuiltCommodityEntry>(entry));
        }
        built[i] = std::move(entry);
    }
    std::vector<BuiltCommodityEntry> out;
    out.reserve(n);
    for (auto& b : built) {
        out.push_back(std::move(*b));
    }
    return out;
}

CommodityCurveBuildResultSet CommodityCurveBuildEvaluator::evaluate(
    const CommodityCurveBuildInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    CommodityCurveSetOptions options;
    options.includeDetails = inputs.includeDetails;
    auto built =
        BuildCommodityCurveSet(inputs.builds, inputs.asOf, reg.rates, &ctx.budget, options);
    CommodityCurveBuildResultSet result;
    result.curves.reserve(built.size());
    for (auto& b : built) {
        result.curves.push_back(std::move(b.row));
    }
    return result;
}

} // namespace quantra
