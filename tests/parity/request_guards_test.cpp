// Request guards restore QuantLib's process-global state in their destructors.
// Restoring notifies observers, and QuantLib rethrows an observer's failure
// once every observer has been notified. An exception leaving a destructor
// calls std::terminate, which kills the worker; the guards must log the
// failure and carry on. The failing cases run in a death-test child, so a
// regression fails one test instead of aborting the whole binary.
#include "eval_date_guard.h"
#include "fixings_guard.h"

#include <ql/indexes/ibor/euribor.hpp>
#include <ql/indexes/indexmanager.hpp>
#include <ql/patterns/observable.hpp>
#include <ql/settings.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace quantra { namespace testing { namespace {

/// Observer whose update() throws while armed.
class ThrowingObserver : public QuantLib::Observer {
public:
    bool armed = false;
    void update() override {
        if (armed) throw std::runtime_error("observer update failed");
    }
};

const QuantLib::Date kOutside(14, QuantLib::June, 2024); // a TARGET business day
const QuantLib::Date kInside(1, QuantLib::July, 2025);

TEST(RequestGuardsDeathTest, EvalDateGuardRestoresTheDateWhenAnObserverThrows) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            QuantLib::Settings::instance().evaluationDate() = kOutside;
            ThrowingObserver observer;
            observer.registerWith(QuantLib::Settings::instance().evaluationDate());
            {
                EvalDateGuard guard;
                QuantLib::Settings::instance().evaluationDate() = kInside;
                observer.armed = true;
            } // restoring kOutside notifies the observer, which throws
            observer.armed = false;
            const bool restored =
                QuantLib::Settings::instance().evaluationDate().value() == kOutside;
            std::exit(restored ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "EvalDateGuard");
}

TEST(RequestGuardsDeathTest, FixingsGuardSurvivesAnObserverFailure) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            auto index = std::make_shared<QuantLib::Euribor6M>();
            ThrowingObserver observer;
            observer.registerWith(index);
            {
                FixingsGuard guard;
                index->addFixing(kOutside, 0.03);
                observer.armed = true;
            } // clearing the fixing notifies the index, then the observer
            observer.armed = false;
            // Whatever the failed clear left behind, the next guard starts clean.
            FixingsGuard next;
            std::exit(QuantLib::IndexManager::instance().histories().empty() ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "FixingsGuard");
}

// An unset evaluation date means "today"; the guard must restore it unset
// rather than pin it to the day the guard was built.
TEST(RequestGuardsTest, EvalDateGuardRestoresAnUnsetDateAsUnset) {
    QuantLib::SavedSettings restore;
    QuantLib::Settings::instance().evaluationDate() = QuantLib::Date();
    {
        EvalDateGuard guard;
        QuantLib::Settings::instance().evaluationDate() = kInside;
    }
    EXPECT_EQ(QuantLib::Settings::instance().evaluationDate().value(), QuantLib::Date());
}

}}} // namespace quantra::testing
