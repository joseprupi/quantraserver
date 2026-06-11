#ifndef QUANTRASERVER_CURVE_SERIALIZER_H
#define QUANTRASERVER_CURVE_SERIALIZER_H

#include <memory>
#include <vector>
#include <string>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/discountcurve.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/time/date.hpp>

#include "curve_cache.h"
#include "enum_convert.h"
#include "date_convert.h"
#include "term_structure_generated.h"

namespace quantra {

/**
 * CurveSerializer - Converts between live QuantLib curves and CachedCurveData.
 *
 * Serialization: extracts ACTUAL pillar dates + discount factors from a
 * bootstrapped PiecewiseYieldCurve. This is critical for exact reconstruction —
 * at pillar nodes the values match exactly, and between pillars the same
 * interpolator reproduces identical values.
 *
 * Reconstruction: builds an InterpolatedDiscountCurve from cached DFs.
 */
class CurveSerializer {
public:

    /**
     * Extract pillar dates + DFs from a bootstrapped curve.
     *
     * Attempts to extract actual pillar nodes from PiecewiseYieldCurve by
     * trying all supported (Trait, Interpolator) combinations. Falls back to
     * dense sampling only if downcast fails (shouldn't happen for curves
     * built by TermStructureParser).
     */
    static CachedCurveData serialize(
        const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
        const quantra::TermStructure* ts)
    {
        CachedCurveData data;

        // Metadata
        auto refDate = curve->referenceDate();
        data.reference_date = DateToIso(refDate);
        data.day_counter = static_cast<uint8_t>(ts->day_counter());
        data.interpolator = static_cast<uint8_t>(ts->interpolator());

        // Try to extract actual pillar nodes from PiecewiseYieldCurve
        std::vector<QuantLib::Date> pillarDates;
        bool extracted = tryExtractPillars(curve, ts, pillarDates);

        if (!extracted) {
            // Fallback: dense sampling (shouldn't normally happen)
            pillarDates = generateFallbackDates(curve);
        }

        // Store dates and discount factors at pillars
        data.dates.reserve(pillarDates.size());
        data.discount_factors.reserve(pillarDates.size());

        for (const auto& dt : pillarDates) {
            data.dates.push_back(DateToIso(dt));
            data.discount_factors.push_back(curve->discount(dt));
        }

        return data;
    }

    /**
     * Reconstruct a QuantLib YieldTermStructure from cached data.
     *
     * Builds an InterpolatedDiscountCurve using the stored pillar DFs.
     * Always stores/reconstructs DFs regardless of original bootstrap trait.
     */
    static std::shared_ptr<QuantLib::YieldTermStructure> reconstruct(
        const CachedCurveData& data)
    {
        std::vector<QuantLib::Date> dates;
        dates.reserve(data.dates.size());
        for (const auto& ds : data.dates) {
            dates.push_back(DateToQL(ds));
        }

        QuantLib::DayCounter dc = DayCounterToQL(
            static_cast<quantra::enums::DayCounter>(data.day_counter));

        auto interpolator = static_cast<quantra::enums::Interpolator>(data.interpolator);

        std::shared_ptr<QuantLib::YieldTermStructure> curve;

        switch (interpolator) {
        case quantra::enums::Interpolator_LogLinear:
            curve = std::make_shared<
                QuantLib::InterpolatedDiscountCurve<QuantLib::LogLinear>>(
                dates, data.discount_factors, dc);
            break;

        case quantra::enums::Interpolator_Linear:
            curve = std::make_shared<
                QuantLib::InterpolatedDiscountCurve<QuantLib::Linear>>(
                dates, data.discount_factors, dc);
            break;

        case quantra::enums::Interpolator_BackwardFlat:
            curve = std::make_shared<
                QuantLib::InterpolatedDiscountCurve<QuantLib::BackwardFlat>>(
                dates, data.discount_factors, dc);
            break;

        case quantra::enums::Interpolator_ForwardFlat:
            // ForwardFlat on DFs doesn't exist directly — LogLinear is the
            // standard DF interpolation and produces identical pillars
            curve = std::make_shared<
                QuantLib::InterpolatedDiscountCurve<QuantLib::LogLinear>>(
                dates, data.discount_factors, dc);
            break;

        case quantra::enums::Interpolator_LogCubic:
            curve = std::make_shared<
                QuantLib::InterpolatedDiscountCurve<QuantLib::LogCubic>>(
                dates, data.discount_factors, dc, QuantLib::MonotonicLogCubic());
            break;

        default:
            curve = std::make_shared<
                QuantLib::InterpolatedDiscountCurve<QuantLib::LogLinear>>(
                dates, data.discount_factors, dc);
            break;
        }

        curve->enableExtrapolation();
        return curve;
    }

    // -------------------------------------------------------------------------
    // Byte (de)serialization for L2 (Redis) storage.
    //
    // Compact, versioned binary layout. Discount factors are written as raw
    // IEEE-754 doubles (memcpy) so reconstruction is BIT-EXACT — no decimal
    // round-tripping. Producer and consumer share the same architecture (same
    // container image), so raw little-endian double layout is portable here.
    //
    //   magic    : 4 bytes  "QLC1"
    //   ref_date : u32 len + bytes
    //   day_counter, interpolator : u8 each
    //   n        : u32 (pillar count)
    //   dates[n] : (u32 len + bytes) each
    //   dfs[n]   : 8 raw bytes each
    // -------------------------------------------------------------------------
    static std::string encode(const CachedCurveData& data) {
        std::string out;
        out.reserve(32 + data.dates.size() * 24);
        appendBytes(out, "QLC1", 4);
        appendString(out, data.reference_date);
        out.push_back(static_cast<char>(data.day_counter));
        out.push_back(static_cast<char>(data.interpolator));
        appendU32(out, static_cast<uint32_t>(data.dates.size()));
        for (const auto& d : data.dates) {
            appendString(out, d);
        }
        for (double df : data.discount_factors) {
            appendBytes(out, reinterpret_cast<const char*>(&df), sizeof(double));
        }
        return out;
    }

    // Throws std::runtime_error on malformed input (caller treats as a miss).
    static CachedCurveData decode(const std::string& blob) {
        size_t pos = 0;
        if (blob.size() < 4 || std::memcmp(blob.data(), "QLC1", 4) != 0) {
            throw std::runtime_error("CurveSerializer::decode: bad magic");
        }
        pos += 4;

        CachedCurveData data;
        data.reference_date = readString(blob, pos);
        data.day_counter = static_cast<uint8_t>(readU8(blob, pos));
        data.interpolator = static_cast<uint8_t>(readU8(blob, pos));

        uint32_t n = readU32(blob, pos);
        data.dates.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            data.dates.push_back(readString(blob, pos));
        }
        data.discount_factors.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            data.discount_factors.push_back(readDouble(blob, pos));
        }
        return data;
    }

private:

    static void appendBytes(std::string& out, const char* p, size_t n) {
        out.append(p, n);
    }
    static void appendU32(std::string& out, uint32_t v) {
        char buf[4];
        std::memcpy(buf, &v, 4);
        out.append(buf, 4);
    }
    static void appendString(std::string& out, const std::string& s) {
        appendU32(out, static_cast<uint32_t>(s.size()));
        out.append(s);
    }

    static void need(const std::string& blob, size_t pos, size_t n) {
        if (pos + n > blob.size()) {
            throw std::runtime_error("CurveSerializer::decode: truncated input");
        }
    }
    static uint8_t readU8(const std::string& blob, size_t& pos) {
        need(blob, pos, 1);
        return static_cast<uint8_t>(blob[pos++]);
    }
    static uint32_t readU32(const std::string& blob, size_t& pos) {
        need(blob, pos, 4);
        uint32_t v;
        std::memcpy(&v, blob.data() + pos, 4);
        pos += 4;
        return v;
    }
    static double readDouble(const std::string& blob, size_t& pos) {
        need(blob, pos, sizeof(double));
        double v;
        std::memcpy(&v, blob.data() + pos, sizeof(double));
        pos += sizeof(double);
        return v;
    }
    static std::string readString(const std::string& blob, size_t& pos) {
        uint32_t len = readU32(blob, pos);
        need(blob, pos, len);
        std::string s(blob.data() + pos, len);
        pos += len;
        return s;
    }

    /**
     * Try to extract actual pillar dates from a PiecewiseYieldCurve.
     *
     * PiecewiseYieldCurve<Trait, Interp>::dates() returns the bootstrap nodes.
     * We try the (trait, interpolator) combo specified in the TermStructure.
     */
    static bool tryExtractPillars(
        const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
        const quantra::TermStructure* ts,
        std::vector<QuantLib::Date>& out)
    {
        #define TRY_EXTRACT(TraitT, InterpT) \
            { \
                auto ptr = std::dynamic_pointer_cast< \
                    QuantLib::PiecewiseYieldCurve<TraitT, InterpT>>(curve); \
                if (ptr) { \
                    out = ptr->dates(); \
                    return true; \
                } \
            }

        using namespace QuantLib;

        auto interp = ts->interpolator();
        auto trait  = ts->bootstrap_trait();

        // Try the expected combination first (most likely hit)
        switch (interp) {
        case enums::Interpolator_LogLinear:
            if (trait == enums::BootstrapTrait_Discount)  TRY_EXTRACT(Discount, LogLinear);
            if (trait == enums::BootstrapTrait_ZeroRate)   TRY_EXTRACT(ZeroYield, LogLinear);
            if (trait == enums::BootstrapTrait_FwdRate)    TRY_EXTRACT(ForwardRate, LogLinear);
            break;
        case enums::Interpolator_Linear:
            if (trait == enums::BootstrapTrait_Discount)  TRY_EXTRACT(Discount, Linear);
            if (trait == enums::BootstrapTrait_ZeroRate)   TRY_EXTRACT(ZeroYield, Linear);
            if (trait == enums::BootstrapTrait_FwdRate)    TRY_EXTRACT(ForwardRate, Linear);
            break;
        case enums::Interpolator_BackwardFlat:
            if (trait == enums::BootstrapTrait_Discount)  TRY_EXTRACT(Discount, BackwardFlat);
            if (trait == enums::BootstrapTrait_ZeroRate)   TRY_EXTRACT(ZeroYield, BackwardFlat);
            if (trait == enums::BootstrapTrait_FwdRate)    TRY_EXTRACT(ForwardRate, BackwardFlat);
            break;
        case enums::Interpolator_ForwardFlat:
            if (trait == enums::BootstrapTrait_Discount)  TRY_EXTRACT(Discount, ForwardFlat);
            if (trait == enums::BootstrapTrait_ZeroRate)   TRY_EXTRACT(ZeroYield, ForwardFlat);
            if (trait == enums::BootstrapTrait_FwdRate)    TRY_EXTRACT(ForwardRate, ForwardFlat);
            break;
        case enums::Interpolator_LogCubic:
            if (trait == enums::BootstrapTrait_Discount)  TRY_EXTRACT(Discount, LogCubic);
            if (trait == enums::BootstrapTrait_ZeroRate)   TRY_EXTRACT(ZeroYield, LogCubic);
            if (trait == enums::BootstrapTrait_FwdRate)    TRY_EXTRACT(ForwardRate, LogCubic);
            break;
        default:
            break;
        }

        #undef TRY_EXTRACT

        return false;
    }

    /**
     * Fallback: generate dense sample dates if we can't extract pillars.
     * Weekly sampling gives reasonable accuracy.
     */
    static std::vector<QuantLib::Date> generateFallbackDates(
        const std::shared_ptr<QuantLib::YieldTermStructure>& curve)
    {
        std::vector<QuantLib::Date> dates;
        auto refDate = curve->referenceDate();
        auto maxDate = curve->maxDate();

        dates.push_back(refDate);

        QuantLib::Date d = refDate;
        while (d < maxDate) {
            d += QuantLib::Period(1, QuantLib::Weeks);
            if (d > maxDate) d = maxDate;
            dates.push_back(d);
        }

        std::sort(dates.begin(), dates.end());
        dates.erase(std::unique(dates.begin(), dates.end()), dates.end());

        return dates;
    }
};

} // namespace quantra

#endif // QUANTRASERVER_CURVE_SERIALIZER_H
