#include "roll_offset.h"

namespace quantra {

namespace {
// Per-request state of the worker thread that owns the request (see the
// header). thread_local: a request never observes another thread's guard.
thread_local int g_rollOffsetDays = 0;
} // namespace

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
