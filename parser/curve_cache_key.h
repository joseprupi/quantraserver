#ifndef QUANTRASERVER_CURVE_CACHE_KEY_H
#define QUANTRASERVER_CURVE_CACHE_KEY_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <unordered_map>

#include "term_structure_generated.h"
#include "index_generated.h"
#include "quotes_generated.h"
#include "quote_registry.h"

namespace quantra {

/**
 * CanonicalBuffer - Accumulates bytes for deterministic hashing.
 *
 * All multi-byte values are written in little-endian.
 * Floats are written as IEEE-754 bytes with -0.0 normalized to +0.0.
 */
class CanonicalBuffer {
public:
    void writeU8(uint8_t v) { buf_.push_back(v); }
    
    void writeI32(int32_t v) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        writeLEU32(u);
    }

    void writeU32(uint32_t v) { writeLEU32(v); }

    void writeDouble(double v) {
        // Normalize -0.0 to +0.0
        if (v == 0.0) v = 0.0;
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        writeLEU64(bits);
    }

    void writeBool(bool v) { buf_.push_back(v ? 1 : 0); }

    void writeString(const char* s, size_t len) {
        writeU32(static_cast<uint32_t>(len));
        buf_.insert(buf_.end(), s, s + len);
    }

    void writeString(const std::string& s) {
        writeString(s.data(), s.size());
    }

    void writeFbString(const flatbuffers::String* s) {
        if (s) {
            writeString(s->c_str(), s->size());
        } else {
            writeU32(0);
        }
    }

    /// Write a separator tag to distinguish sections
    void writeTag(const char* tag) {
        size_t len = std::strlen(tag);
        buf_.insert(buf_.end(), tag, tag + len);
    }

    /// Append raw bytes
    void appendBytes(const std::vector<uint8_t>& bytes) {
        buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    }

    const std::vector<uint8_t>& data() const { return buf_; }
    size_t size() const { return buf_.size(); }

private:
    void writeLEU32(uint32_t v) {
        buf_.push_back(static_cast<uint8_t>(v & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    }

    void writeLEU64(uint64_t v) {
        for (int i = 0; i < 8; i++) {
            buf_.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
        }
    }

    std::vector<uint8_t> buf_;
};

/// Pre-built lookup maps for O(1) quote and index resolution during key computation.
struct KeyContext {
    /// quote_id → resolved numeric value (built from QuoteSpec array)
    std::unordered_map<std::string, double> quoteValues;

    /// index_id → pointer to IndexDef (built from indices array)
    std::unordered_map<std::string, const quantra::IndexDef*> indexDefs;

    /// Build from raw FlatBuffer arrays (call once per bootstrapAll)
    static KeyContext build(
        const flatbuffers::Vector<flatbuffers::Offset<quantra::QuoteSpec>>* quotes,
        const flatbuffers::Vector<flatbuffers::Offset<quantra::IndexDef>>* indices)
    {
        KeyContext ctx;
        if (quotes) {
            for (flatbuffers::uoffset_t i = 0; i < quotes->size(); i++) {
                auto q = quotes->Get(i);
                if (q->id() && q->quote_type() == quantra::QuoteType_Curve) {
                    ctx.quoteValues[q->id()->str()] = q->value();
                }
            }
        }
        if (indices) {
            for (flatbuffers::uoffset_t i = 0; i < indices->size(); i++) {
                auto def = indices->Get(i);
                if (def->id()) {
                    ctx.indexDefs[def->id()->str()] = def;
                }
            }
        }
        return ctx;
    }
};


/**
 * CurveKeyBuilder - Builds deterministic cache keys for yield curve specs.
 *
 * Produces: "yc:v1:<sha256hex>"
 *
 * The key captures everything that affects bootstrapping output:
 * - as_of_date
 * - TermStructure config (day counter, interpolator, bootstrap trait, reference date)
 * - All helpers with their full field values (sorted for order-independence)
 * - Resolved quote values (not quote IDs)
 * - Index definitions referenced by helpers (full conventions)
 * - Dependency curve keys (for multi-curve bootstrap)
 */
class CurveKeyBuilder {
public:
    /**
     * Compute cache key for a single TermStructure.
     *
     * @param asOfDate      Evaluation date string (YYYY-MM-DD)
     * @param ts            The curve spec
     * @param ctx           Pre-built quote/index lookup maps
     * @param depKeys       Keys of dependency curves (sorted by depId)
     * @return              Key string "yc:v1:<sha256hex>"
     */
    static std::string compute(
        const std::string& asOfDate,
        const quantra::TermStructure* ts,
        const KeyContext& ctx,
        const std::map<std::string, std::string>& depKeys);

private:
    static void writeCurveHeader(
        CanonicalBuffer& buf,
        const std::string& asOfDate,
        const quantra::TermStructure* ts);

    static std::vector<uint8_t> serializePoint(
        const quantra::PointsWrapper* pw,
        const KeyContext& ctx);

    static void writeReferencedIndices(
        CanonicalBuffer& buf,
        const quantra::TermStructure* ts,
        const KeyContext& ctx);

    /// Resolve a quote_id to its numeric value via O(1) map lookup
    static double resolveQuoteValue(
        double inlineValue,
        const flatbuffers::String* quoteId,
        const KeyContext& ctx);

    static void writeDeps(
        CanonicalBuffer& buf,
        const quantra::HelperDependencies* deps);

    static void writeIndexRef(
        CanonicalBuffer& buf,
        const quantra::IndexRef* ref);

    static void writeSchedule(
        CanonicalBuffer& buf,
        const quantra::Schedule* sched);

    static std::string sha256hex(const std::vector<uint8_t>& data);
};

} // namespace quantra

#endif // QUANTRASERVER_CURVE_CACHE_KEY_H
