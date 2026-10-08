#include "roll_offset.h"

namespace quantra {

namespace {
int g_rollOffsetDays = 0;
}

int activeRollOffsetDays() {
    return g_rollOffsetDays;
}

QuantLib::Date applyRollOffset(const QuantLib::Date& explicitReference) {
    return explicitReference + g_rollOffsetDays;
}

RollOffsetGuard::RollOffsetGuard(int days) {
    g_rollOffsetDays = days;
}

RollOffsetGuard::~RollOffsetGuard() {
    g_rollOffsetDays = 0;
}

} // namespace quantra
