#ifndef QUANTRASERVER_BOOTSTRAP_CURVES_REQUEST_H
#define QUANTRASERVER_BOOTSTRAP_CURVES_REQUEST_H

#include <ql/quantlib.hpp>
#include "flatbuffers/grpc.h"
#include "bootstrap_curves_request_generated.h"
#include "bootstrap_curves_response_generated.h"
#include "term_structure_parser.h"
#include "term_structure_point_parser.h"
#include "curve_bootstrapper.h"
#include "common_parser.h"
#include "error.h"

/**
 * BootstrapCurvesRequestHandler - Bootstraps yield curves and returns sampled measures.
 * 
 * CHANGE: Now uses CurveBootstrapper for dependency-aware multi-curve bootstrapping.
 * If curves reference each other via HelperDependencies (e.g., Euribor curve
 * discounted with OIS), the bootstrapper handles the ordering automatically.
 * 
 * Supports:
 * - Multiple curves in a single request (with inter-curve dependencies)
 * - Measures: DF (discount factor), ZERO (zero rate), FWD (forward rate)
 * - Grid types: TenorGrid (structured {n, unit} tenors) or RangeGrid (daily/weekly sampling)
 * - Configurable zero rate conventions (compounding, frequency, day counter)
 * - Configurable forward rate conventions (instantaneous or period forward)
 */
class BootstrapCurvesRequestHandler
{
public:
    flatbuffers::Offset<quantra::BootstrapCurvesResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::BootstrapCurvesRequest *request) const;

private:
    // Build grid dates from TenorGrid (uses structured Tenor with {n, unit})
    std::vector<QuantLib::Date> buildTenorGrid(
        const quantra::TenorGrid *grid,
        const QuantLib::Date &referenceDate,
        const QuantLib::Calendar &fallbackCalendar) const;

    // Build grid dates from RangeGrid
    std::vector<QuantLib::Date> buildRangeGrid(
        const quantra::RangeGrid *grid,
        const QuantLib::Date &asOfDate) const;

    // Compute discount factors
    std::vector<double> computeDiscountFactors(
        const std::shared_ptr<QuantLib::YieldTermStructure> &curve,
        const std::vector<QuantLib::Date> &dates) const;

    // Compute zero rates
    std::vector<double> computeZeroRates(
        const std::shared_ptr<QuantLib::YieldTermStructure> &curve,
        const std::vector<QuantLib::Date> &dates,
        const quantra::ZeroRateQuery *zeroQuery) const;

    // Compute forward rates
    std::vector<double> computeForwardRates(
        const std::shared_ptr<QuantLib::YieldTermStructure> &curve,
        const std::vector<QuantLib::Date> &dates,
        const quantra::ForwardRateQuery *fwdQuery,
        const QuantLib::Calendar &gridCalendar,
        QuantLib::BusinessDayConvention gridBdc,
        const QuantLib::Calendar &curveCalendar) const;
    
    // Get calendar from term structure (extracts from first point if available)
    QuantLib::Calendar getCalendarFromTermStructure(
        const quantra::TermStructure *termStructure) const;
    
    // Get calendar from grid spec (TenorGrid or RangeGrid)
    QuantLib::Calendar getCalendarFromGrid(
        const quantra::CurveGridSpec *gridSpec,
        const QuantLib::Calendar &fallbackCalendar) const;
    
    // Get BDC from grid spec
    QuantLib::BusinessDayConvention getBdcFromGrid(
        const quantra::CurveGridSpec *gridSpec) const;

    // Extract pillar dates from the TermStructure helpers
    std::vector<QuantLib::Date> extractPillarDatesFromHelpers(
        const quantra::TermStructure *termStructure,
        const QuantLib::Date &referenceDate) const;
};

#endif
