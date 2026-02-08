#include "curve_cache_key.h"

#include <sstream>
#include <iomanip>

#include <openssl/sha.h>

namespace quantra {

// =============================================================================
// SHA-256
// =============================================================================

std::string CurveKeyBuilder::sha256hex(const std::vector<uint8_t>& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

// =============================================================================
// Quote resolution — O(1) via KeyContext map
// =============================================================================

double CurveKeyBuilder::resolveQuoteValue(
    double inlineValue,
    const flatbuffers::String* quoteId,
    const KeyContext& ctx)
{
    if (quoteId) {
        std::string id = quoteId->str();
        if (!id.empty()) {
            auto it = ctx.quoteValues.find(id);
            if (it != ctx.quoteValues.end()) {
                return it->second;
            }
        }
    }
    return inlineValue;
}

// =============================================================================
// Sub-structure writers
// =============================================================================

void CurveKeyBuilder::writeDeps(
    CanonicalBuffer& buf,
    const quantra::HelperDependencies* deps)
{
    if (!deps) {
        buf.writeU8(0);
        return;
    }
    buf.writeU8(1);

    if (deps->discount_curve() && deps->discount_curve()->id()) {
        buf.writeU8(1);
        buf.writeFbString(deps->discount_curve()->id());
    } else {
        buf.writeU8(0);
    }

    if (deps->projection_curve() && deps->projection_curve()->id()) {
        buf.writeU8(1);
        buf.writeFbString(deps->projection_curve()->id());
    } else {
        buf.writeU8(0);
    }

    if (deps->projection_curve_2() && deps->projection_curve_2()->id()) {
        buf.writeU8(1);
        buf.writeFbString(deps->projection_curve_2()->id());
    } else {
        buf.writeU8(0);
    }

    buf.writeFbString(deps->fx_spot_quote_id());
}

void CurveKeyBuilder::writeIndexRef(
    CanonicalBuffer& buf,
    const quantra::IndexRef* ref)
{
    if (ref && ref->id()) {
        buf.writeU8(1);
        buf.writeFbString(ref->id());
    } else {
        buf.writeU8(0);
    }
}

void CurveKeyBuilder::writeSchedule(
    CanonicalBuffer& buf,
    const quantra::Schedule* sched)
{
    if (!sched) {
        buf.writeU8(0);
        return;
    }
    buf.writeU8(1);
    buf.writeU8(static_cast<uint8_t>(sched->calendar()));
    buf.writeFbString(sched->effective_date());
    buf.writeFbString(sched->termination_date());
    buf.writeU8(static_cast<uint8_t>(sched->frequency()));
    buf.writeU8(static_cast<uint8_t>(sched->convention()));
    buf.writeU8(static_cast<uint8_t>(sched->termination_date_convention()));
    buf.writeU8(static_cast<uint8_t>(sched->date_generation_rule()));
    buf.writeBool(sched->end_of_month());
}

// =============================================================================
// Serialize a single helper/point into independent bytes
// =============================================================================

std::vector<uint8_t> CurveKeyBuilder::serializePoint(
    const quantra::PointsWrapper* pw,
    const KeyContext& ctx)
{
    CanonicalBuffer buf;

    auto ptype = pw->point_type();
    buf.writeU8(static_cast<uint8_t>(ptype));

    switch (ptype) {

    case quantra::Point_DepositHelper: {
        auto p = pw->point_as_DepositHelper();
        buf.writeDouble(resolveQuoteValue(p->rate(), p->quote_id(), ctx));
        buf.writeU8(static_cast<uint8_t>(p->tenor_time_unit()));
        buf.writeI32(p->tenor_number());
        buf.writeI32(p->fixing_days());
        buf.writeU8(static_cast<uint8_t>(p->calendar()));
        buf.writeU8(static_cast<uint8_t>(p->business_day_convention()));
        buf.writeU8(static_cast<uint8_t>(p->day_counter()));
        break;
    }

    case quantra::Point_FRAHelper: {
        auto p = pw->point_as_FRAHelper();
        buf.writeDouble(resolveQuoteValue(p->rate(), p->quote_id(), ctx));
        buf.writeI32(p->months_to_start());
        buf.writeI32(p->months_to_end());
        buf.writeI32(p->fixing_days());
        buf.writeU8(static_cast<uint8_t>(p->calendar()));
        buf.writeU8(static_cast<uint8_t>(p->business_day_convention()));
        buf.writeU8(static_cast<uint8_t>(p->day_counter()));
        break;
    }

    case quantra::Point_FutureHelper: {
        auto p = pw->point_as_FutureHelper();
        double rateValue = p->rate();
        if (p->futures_price() != 0.0) {
            rateValue = 1.0 - (p->futures_price() / 100.0);
        }
        rateValue += p->convexity_adjustment();
        buf.writeDouble(resolveQuoteValue(rateValue, p->quote_id(), ctx));
        buf.writeFbString(p->future_start_date());
        buf.writeI32(p->future_months());
        buf.writeU8(static_cast<uint8_t>(p->calendar()));
        buf.writeU8(static_cast<uint8_t>(p->business_day_convention()));
        buf.writeU8(static_cast<uint8_t>(p->day_counter()));
        buf.writeDouble(p->futures_price());
        buf.writeDouble(p->convexity_adjustment());
        break;
    }

    case quantra::Point_SwapHelper: {
        auto p = pw->point_as_SwapHelper();
        buf.writeDouble(resolveQuoteValue(p->rate(), p->quote_id(), ctx));
        buf.writeU8(static_cast<uint8_t>(p->tenor_time_unit()));
        buf.writeI32(p->tenor_number());
        buf.writeU8(static_cast<uint8_t>(p->calendar()));
        buf.writeU8(static_cast<uint8_t>(p->sw_fixed_leg_frequency()));
        buf.writeU8(static_cast<uint8_t>(p->sw_fixed_leg_convention()));
        buf.writeU8(static_cast<uint8_t>(p->sw_fixed_leg_day_counter()));
        writeIndexRef(buf, p->float_index());
        buf.writeDouble(p->spread());
        buf.writeI32(p->fwd_start_days());
        writeDeps(buf, p->deps());
        break;
    }

    case quantra::Point_BondHelper: {
        auto p = pw->point_as_BondHelper();
        double px = p->price();
        if (px == 0.0) px = p->rate();
        buf.writeDouble(resolveQuoteValue(px, p->quote_id(), ctx));
        buf.writeI32(p->settlement_days());
        buf.writeDouble(p->face_amount());
        writeSchedule(buf, p->schedule());
        buf.writeDouble(p->coupon_rate());
        buf.writeU8(static_cast<uint8_t>(p->day_counter()));
        buf.writeU8(static_cast<uint8_t>(p->business_day_convention()));
        buf.writeDouble(p->redemption());
        buf.writeFbString(p->issue_date());
        break;
    }

    case quantra::Point_OISHelper: {
        auto p = pw->point_as_OISHelper();
        buf.writeDouble(resolveQuoteValue(p->rate(), p->quote_id(), ctx));
        buf.writeI32(p->tenor_number());
        buf.writeU8(static_cast<uint8_t>(p->tenor_time_unit()));
        writeIndexRef(buf, p->overnight_index());
        buf.writeI32(p->settlement_days());
        buf.writeU8(static_cast<uint8_t>(p->calendar()));
        buf.writeU8(static_cast<uint8_t>(p->fixed_leg_frequency()));
        buf.writeU8(static_cast<uint8_t>(p->fixed_leg_convention()));
        buf.writeU8(static_cast<uint8_t>(p->fixed_leg_day_counter()));
        writeDeps(buf, p->deps());
        break;
    }

    case quantra::Point_DatedOISHelper: {
        auto p = pw->point_as_DatedOISHelper();
        buf.writeDouble(resolveQuoteValue(p->rate(), p->quote_id(), ctx));
        buf.writeFbString(p->start_date());
        buf.writeFbString(p->end_date());
        writeIndexRef(buf, p->overnight_index());
        buf.writeI32(p->settlement_days());
        buf.writeU8(static_cast<uint8_t>(p->calendar()));
        buf.writeU8(static_cast<uint8_t>(p->fixed_leg_convention()));
        buf.writeU8(static_cast<uint8_t>(p->fixed_leg_day_counter()));
        writeDeps(buf, p->deps());
        break;
    }

    case quantra::Point_TenorBasisSwapHelper: {
        auto p = pw->point_as_TenorBasisSwapHelper();
        buf.writeDouble(resolveQuoteValue(p->spread(), p->quote_id(), ctx));
        buf.writeI32(p->tenor_number());
        buf.writeU8(static_cast<uint8_t>(p->tenor_time_unit()));
        writeIndexRef(buf, p->index_short());
        writeIndexRef(buf, p->index_long());
        buf.writeU8(static_cast<uint8_t>(p->calendar()));
        writeDeps(buf, p->deps());
        break;
    }

    case quantra::Point_FxSwapHelper: {
        auto p = pw->point_as_FxSwapHelper();
        buf.writeDouble(resolveQuoteValue(p->fx_points(), p->quote_id(), ctx));
        buf.writeI32(p->tenor_number());
        buf.writeU8(static_cast<uint8_t>(p->tenor_time_unit()));
        buf.writeI32(p->spot_days());
        buf.writeU8(static_cast<uint8_t>(p->calendar_domestic()));
        buf.writeU8(static_cast<uint8_t>(p->calendar_foreign()));
        writeDeps(buf, p->deps());
        break;
    }

    case quantra::Point_CrossCcyBasisHelper: {
        auto p = pw->point_as_CrossCcyBasisHelper();
        buf.writeDouble(resolveQuoteValue(p->spread(), p->quote_id(), ctx));
        buf.writeI32(p->tenor_number());
        buf.writeU8(static_cast<uint8_t>(p->tenor_time_unit()));
        writeIndexRef(buf, p->index_domestic());
        writeIndexRef(buf, p->index_foreign());
        writeDeps(buf, p->deps());
        break;
    }

    default:
        break;
    }

    return buf.data();
}

// =============================================================================
// Write index definitions referenced by this curve — O(1) lookups via ctx
// =============================================================================

void CurveKeyBuilder::writeReferencedIndices(
    CanonicalBuffer& buf,
    const quantra::TermStructure* ts,
    const KeyContext& ctx)
{
    if (!ts->points()) return;

    // Collect all index IDs referenced by helpers in this curve
    std::set<std::string> referencedIds;
    for (flatbuffers::uoffset_t i = 0; i < ts->points()->size(); i++) {
        auto pw = ts->points()->Get(i);
        auto ptype = pw->point_type();

        if (ptype == quantra::Point_SwapHelper) {
            auto p = pw->point_as_SwapHelper();
            if (p->float_index() && p->float_index()->id())
                referencedIds.insert(p->float_index()->id()->str());
        }
        else if (ptype == quantra::Point_OISHelper) {
            auto p = pw->point_as_OISHelper();
            if (p->overnight_index() && p->overnight_index()->id())
                referencedIds.insert(p->overnight_index()->id()->str());
        }
        else if (ptype == quantra::Point_DatedOISHelper) {
            auto p = pw->point_as_DatedOISHelper();
            if (p->overnight_index() && p->overnight_index()->id())
                referencedIds.insert(p->overnight_index()->id()->str());
        }
        else if (ptype == quantra::Point_TenorBasisSwapHelper) {
            auto p = pw->point_as_TenorBasisSwapHelper();
            if (p->index_short() && p->index_short()->id())
                referencedIds.insert(p->index_short()->id()->str());
            if (p->index_long() && p->index_long()->id())
                referencedIds.insert(p->index_long()->id()->str());
        }
        else if (ptype == quantra::Point_CrossCcyBasisHelper) {
            auto p = pw->point_as_CrossCcyBasisHelper();
            if (p->index_domestic() && p->index_domestic()->id())
                referencedIds.insert(p->index_domestic()->id()->str());
            if (p->index_foreign() && p->index_foreign()->id())
                referencedIds.insert(p->index_foreign()->id()->str());
        }
    }

    // Write referenced index definitions (std::set gives sorted order)
    buf.writeTag("IDX");
    buf.writeU32(static_cast<uint32_t>(referencedIds.size()));

    for (const auto& refId : referencedIds) {
        // O(1) lookup via context map
        auto it = ctx.indexDefs.find(refId);
        if (it == ctx.indexDefs.end()) continue; // index not found

        const auto* def = it->second;
        buf.writeFbString(def->id());
        buf.writeFbString(def->name());
        buf.writeU8(static_cast<uint8_t>(def->index_type()));
        buf.writeI32(def->tenor_number());
        buf.writeU8(static_cast<uint8_t>(def->tenor_time_unit()));
        buf.writeI32(def->fixing_days());
        buf.writeU8(static_cast<uint8_t>(def->calendar()));
        buf.writeU8(static_cast<uint8_t>(def->business_day_convention()));
        buf.writeU8(static_cast<uint8_t>(def->day_counter()));
        buf.writeBool(def->end_of_month());
        buf.writeFbString(def->currency());

        // Include fixings in key (they affect bootstrap)
        if (def->fixings()) {
            buf.writeU32(def->fixings()->size());
            // Sort fixings by date for determinism
            std::vector<std::pair<std::string, double>> fixings;
            fixings.reserve(def->fixings()->size());
            for (flatbuffers::uoffset_t f = 0; f < def->fixings()->size(); f++) {
                auto fix = def->fixings()->Get(f);
                fixings.emplace_back(
                    fix->date() ? fix->date()->str() : "",
                    fix->value()
                );
            }
            std::sort(fixings.begin(), fixings.end());
            for (const auto& [date, value] : fixings) {
                buf.writeString(date);
                buf.writeDouble(value);
            }
        } else {
            buf.writeU32(0);
        }
    }
}

// =============================================================================
// Curve header
// =============================================================================

void CurveKeyBuilder::writeCurveHeader(
    CanonicalBuffer& buf,
    const std::string& asOfDate,
    const quantra::TermStructure* ts)
{
    buf.writeTag("yc-key-v1");
    buf.writeString(asOfDate);
    buf.writeU8(static_cast<uint8_t>(ts->day_counter()));
    buf.writeU8(static_cast<uint8_t>(ts->interpolator()));
    buf.writeU8(static_cast<uint8_t>(ts->bootstrap_trait()));
    buf.writeFbString(ts->reference_date());
}

// =============================================================================
// Main compute
// =============================================================================

std::string CurveKeyBuilder::compute(
    const std::string& asOfDate,
    const quantra::TermStructure* ts,
    const KeyContext& ctx,
    const std::map<std::string, std::string>& depKeys)
{
    CanonicalBuffer buf;

    // 1. Curve header
    writeCurveHeader(buf, asOfDate, ts);

    // 2. Index definitions (sorted by id)
    writeReferencedIndices(buf, ts, ctx);

    // 3. Helpers — serialize each independently, then sort byte arrays
    buf.writeTag("PTS");
    if (ts->points()) {
        std::vector<std::vector<uint8_t>> helperBlobs;
        helperBlobs.reserve(ts->points()->size());

        for (flatbuffers::uoffset_t i = 0; i < ts->points()->size(); i++) {
            helperBlobs.push_back(serializePoint(ts->points()->Get(i), ctx));
        }

        // Sort helper blobs lexicographically for order-independence
        std::sort(helperBlobs.begin(), helperBlobs.end());

        buf.writeU32(static_cast<uint32_t>(helperBlobs.size()));
        for (const auto& blob : helperBlobs) {
            buf.writeU32(static_cast<uint32_t>(blob.size()));
            buf.appendBytes(blob);
        }
    } else {
        buf.writeU32(0);
    }

    // 4. Dependency curve keys (std::map already sorted by depId)
    buf.writeTag("DEP");
    buf.writeU32(static_cast<uint32_t>(depKeys.size()));
    for (const auto& [depId, depKey] : depKeys) {
        buf.writeString(depId);
        buf.writeString(depKey);
    }

    // 5. Hash
    return "yc:v1:" + sha256hex(buf.data());
}

} // namespace quantra
