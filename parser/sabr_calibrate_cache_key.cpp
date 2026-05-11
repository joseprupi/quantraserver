#include "sabr_calibrate_cache_key.h"

#include <openssl/sha.h>

#include <iomanip>
#include <sstream>

#include "curve_cache_key.h"

namespace quantra {

namespace {

std::string sha256hex(const std::vector<uint8_t>& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

void writePeriods(CanonicalBuffer& buf, const std::vector<QuantLib::Period>& periods) {
    buf.writeU32(static_cast<uint32_t>(periods.size()));
    for (const auto& p : periods) {
        buf.writeI32(p.length());
        // QuantLib::TimeUnit is an enum; canonicalize via int cast.
        buf.writeU8(static_cast<uint8_t>(p.units()));
    }
}

void writeDoubles(CanonicalBuffer& buf, const std::vector<double>& xs) {
    buf.writeU32(static_cast<uint32_t>(xs.size()));
    for (double v : xs) {
        buf.writeDouble(v);
    }
}

} // namespace

std::string buildSabrCalibrateCacheKey(
    const SwaptionVolEntry& e,
    const std::vector<double>& atmForwardsFlat,
    const std::string& discountCurveKey,
    const std::string& forwardingCurveKey) {
    CanonicalBuffer buf;

    buf.writeTag("sabr-cube:v1:");

    // Surface identity / pillars
    buf.writeI32(e.nExp);
    buf.writeI32(e.nTen);
    buf.writeI32(e.nStrikes);
    writePeriods(buf, e.expiries);
    writePeriods(buf, e.tenors);
    writeDoubles(buf, e.sabrStrikeSpreads);

    // Resolved market vols (post-quote-registry)
    writeDoubles(buf, e.sabrMarketVolsFlat);

    // Calibration options
    buf.writeBool(e.sabrBetaFixed);
    buf.writeDouble(e.sabrBetaValue);
    buf.writeBool(e.sabrVegaWeightedSmileFit);

    // Surface metadata
    buf.writeDouble(e.displacement);
    buf.writeU8(static_cast<uint8_t>(e.qlVolType));
    // Reference date as serial day count (calendar-independent canonical form).
    buf.writeI32(static_cast<int32_t>(e.referenceDate.serialNumber()));

    // Forward conventions (swap index + curve identities). The curve cache
    // keys carry full bootstrap identity, so we delegate to them rather than
    // re-serializing curve internals here.
    buf.writeString(e.swapIndexId);
    buf.writeString(discountCurveKey);
    buf.writeString(forwardingCurveKey);

    // Server-computed forwards. These are deterministic given the curves +
    // swap index above, but including them defends against any path that
    // bypasses the standard finalize.
    writeDoubles(buf, atmForwardsFlat);

    return "sabr-cube:v1:" + sha256hex(buf.data());
}

} // namespace quantra
