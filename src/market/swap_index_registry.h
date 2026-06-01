#ifndef QUANTRA_SWAP_INDEX_REGISTRY_H
#define QUANTRA_SWAP_INDEX_REGISTRY_H

#include <string>
#include <unordered_map>

#include <ql/time/calendar.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/period.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/dategenerationrule.hpp>
#include <ql/time/frequency.hpp>
#include <ql/indexes/swapindex.hpp>
#include <ql/handle.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "swap_index_generated.h"
#include "index_registry.h"
#include "enum_convert.h"
#include "error.h"

namespace quantra {

struct SwapIndexRuntime {
    quantra::SwapIndexKind kind = quantra::SwapIndexKind_IborSwapIndex;
    int spotDays = -1;
    QuantLib::Calendar calendar = QuantLib::TARGET();
    QuantLib::BusinessDayConvention bdc = QuantLib::ModifiedFollowing;
    bool endOfMonth = false;

    QuantLib::Frequency fixedFrequency = QuantLib::Annual;
    QuantLib::DayCounter fixedDayCounter = QuantLib::Thirty360(QuantLib::Thirty360::BondBasis);
    QuantLib::Calendar fixedCalendar = QuantLib::TARGET();
    quantra::enums::Calendar fixedCalendarFb = quantra::enums::Calendar_TARGET;
    QuantLib::BusinessDayConvention fixedBdc = QuantLib::ModifiedFollowing;
    quantra::enums::BusinessDayConvention fixedBdcFb = quantra::enums::BusinessDayConvention_ModifiedFollowing;
    QuantLib::BusinessDayConvention fixedTermBdc = QuantLib::ModifiedFollowing;
    QuantLib::DateGeneration::Rule fixedDateRule = QuantLib::DateGeneration::Forward;
    bool fixedEom = false;

    QuantLib::Period floatTenor = QuantLib::Period(6, QuantLib::Months);
    QuantLib::Calendar floatCalendar = QuantLib::TARGET();
    QuantLib::BusinessDayConvention floatBdc = QuantLib::ModifiedFollowing;
    QuantLib::BusinessDayConvention floatTermBdc = QuantLib::ModifiedFollowing;
    QuantLib::DateGeneration::Rule floatDateRule = QuantLib::DateGeneration::Forward;
    bool floatEom = false;

    std::string floatIndexId;
};

class SwapIndexRegistry {
public:
    void add(const std::string& id, const SwapIndexRuntime& v) { data_[id] = v; }
    bool has(const std::string& id) const { return data_.find(id) != data_.end(); }
    const SwapIndexRuntime& get(const std::string& id) const {
        auto it = data_.find(id);
        if (it == data_.end()) {
            QUANTRA_NOT_FOUND("Unknown swap index id: " + id);
        }
        return it->second;
    }

    std::shared_ptr<QuantLib::SwapIndex> getIborSwapIndexWithCurves(
        const std::string& id,
        const QuantLib::Period& tenor,
        const IndexRegistry& indices,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve) const;
private:
    std::unordered_map<std::string, SwapIndexRuntime> data_;
};

class SwapIndexRegistryBuilder {
public:
    SwapIndexRegistry build(
        const flatbuffers::Vector<flatbuffers::Offset<quantra::SwapIndexDef>>* defs,
        const IndexRegistry& indices) const;
};

} // namespace quantra

#endif // QUANTRA_SWAP_INDEX_REGISTRY_H
