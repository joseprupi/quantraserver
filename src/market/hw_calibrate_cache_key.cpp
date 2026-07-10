#include "hw_calibrate_cache_key.h"

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

void writePeriod(CanonicalBuffer& buf, const QuantLib::Period& p) {
    buf.writeI32(p.length());
    // QuantLib::TimeUnit is an enum; canonicalize via int cast.
    buf.writeU8(static_cast<uint8_t>(p.units()));
}

void writePeriods(CanonicalBuffer& buf, const std::vector<QuantLib::Period>& periods) {
    buf.writeU32(static_cast<uint32_t>(periods.size()));
    for (const auto& p : periods) {
        writePeriod(buf, p);
    }
}

void writeDoubles(CanonicalBuffer& buf, const std::vector<double>& xs) {
    buf.writeU32(static_cast<uint32_t>(xs.size()));
    for (double v : xs) {
        buf.writeDouble(v);
    }
}

} // namespace

std::string buildHwCalibrateCacheKey(const HwCalibrateKeyInputs& in) {
    CanonicalBuffer buf;

    buf.writeTag("hw-calib:v1:");

    // Consumed market vols + resolved grid.
    writeDoubles(buf, in.consumedVols);
    writePeriods(buf, in.expiries);
    writePeriods(buf, in.tenors);

    // Vol structure metadata.
    buf.writeU8(static_cast<uint8_t>(in.volType));
    buf.writeDouble(in.displacement);
    buf.writeI32(static_cast<int32_t>(in.volReferenceDate.serialNumber()));

    // Curve identity — delegated to the curve cache keys, which carry the full
    // bootstrap identity (helpers + quotes + dependencies).
    buf.writeString(in.discountCurveKey);
    buf.writeString(in.forwardingCurveKey);

    // Swap-index identity fields consumed by the helper build.
    buf.writeString(in.swapIndexId);
    buf.writeString(in.floatIndexId);
    buf.writeI32(static_cast<int32_t>(in.fixedFrequency));
    buf.writeString(in.fixedDayCounter.empty() ? std::string() : in.fixedDayCounter.name());
    buf.writeI32(in.spotDays);
    buf.writeString(in.fixedCalendar.empty() ? std::string() : in.fixedCalendar.name());
    buf.writeString(in.floatCalendar.empty() ? std::string() : in.floatCalendar.name());

    // IBOR index identity (cloned onto the forwarding curve) as consumed.
    writePeriod(buf, in.iborTenor);
    buf.writeString(in.iborDayCounter.empty() ? std::string() : in.iborDayCounter.name());
    buf.writeU8(static_cast<uint8_t>(in.iborConvention));
    buf.writeBool(in.iborEndOfMonth);
    buf.writeString(in.iborFixingCalendar.empty() ? std::string() : in.iborFixingCalendar.name());

    // Calibration spec (clamped iteration/eval counts).
    buf.writeBool(in.calibrateA);
    buf.writeBool(in.calibrateSigma);
    buf.writeDouble(in.aInit);
    buf.writeDouble(in.sigmaInit);
    buf.writeI32(in.maxIterations);
    buf.writeI32(in.functionEvaluations);
    buf.writeDouble(in.endCriteriaEps);

    // Evaluation date.
    buf.writeI32(static_cast<int32_t>(in.asOf.serialNumber()));

    return "hw-calib:v1:" + sha256hex(buf.data());
}

} // namespace quantra
