#ifndef QUANTRASERVER_BOOTSTRAP_CURVES_REQUEST_H
#define QUANTRASERVER_BOOTSTRAP_CURVES_REQUEST_H

#include <ql/quantlib.hpp>

#include "flatbuffers/grpc.h"
#include "bootstrap_curves_request_generated.h"
#include "bootstrap_curves_response_generated.h"
#include "pricing_registry.h"
#include "common_parser.h"
#include "error.h"

class BootstrapCurvesRequestHandler {
public:
    flatbuffers::Offset<quantra::BootstrapCurvesResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::BootstrapCurvesRequest* request) const;

private:
    std::vector<QuantLib::Date> buildTenorGrid(
        const quantra::TenorGrid* grid,
        const QuantLib::Date& referenceDate,
        const QuantLib::Calendar& fallbackCalendar) const;

    std::vector<QuantLib::Date> buildRangeGrid(
        const quantra::RangeGrid* grid,
        const QuantLib::Date& asOfDate,
        int maxPoints) const;

    std::vector<double> computeDiscountFactors(
        const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
        const std::vector<QuantLib::Date>& dates) const;

    std::vector<double> computeZeroRates(
        const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
        const std::vector<QuantLib::Date>& dates,
        const quantra::ZeroRateQuery* zeroQuery) const;

    std::vector<double> computeForwardRates(
        const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
        const std::vector<QuantLib::Date>& dates,
        const quantra::ForwardRateQuery* fwdQuery,
        const QuantLib::Calendar& gridCalendar,
        QuantLib::BusinessDayConvention gridBdc,
        const QuantLib::Calendar& curveCalendar) const;

    QuantLib::Calendar getCalendarFromTermStructure(
        const quantra::TermStructure* termStructure) const;

    QuantLib::Calendar getCalendarFromGrid(
        const quantra::DateGridSpec* gridSpec,
        const quantra::QueryOptions* options,
        const QuantLib::Calendar& fallbackCalendar) const;

    QuantLib::BusinessDayConvention getBdcFromGrid(
        const quantra::DateGridSpec* gridSpec,
        const quantra::QueryOptions* options) const;

    std::vector<QuantLib::Date> extractPillarDatesFromHelpers(
        const quantra::TermStructure* termStructure,
        const QuantLib::Date& referenceDate) const;
};

#endif
