// native_var_check.cpp — standalone raw-QuantLib cross-check for the
// historical-simulation VaR example (historical_var.py).
//
// Reads the swaps.csv + quotes.csv written by
//   python3 historical_var.py --dump-inputs DIR ...
// and reprices the SAME book on the SAME base + scenario curves with direct
// QuantLib calls — no server, no gRPC — writing native_results.csv:
// first line = base book value, then one P&L per scenario (scenario book
// value minus base book value), full double precision. Feed that file back to
//   python3 historical_var.py --compare-native native_results.csv ...
// to see the server-vs-native differences.
//
// Every market/instrument convention below deliberately mirrors what the
// QuantraServer engine does with the script's JSON:
//   * curve:  PiecewiseYieldCurve<Discount, LogLinear>, reference date
//             2025-01-15, Actual365Fixed, IterativeBootstrap tolerance 1e-15
//             (src/parsers/term_structure_parser.cpp), no extrapolation.
//   * pillar: OISRateHelper exactly as src/parsers/term_structure_point_parser.cpp
//             builds it — settlement days 2, payment lag 2, payment convention
//             ModifiedFollowing, payment frequency Annual, payment calendar
//             UnitedStates(GovernmentBond), spot start, zero overnight spread,
//             Pillar::LastRelevantDate, RateAveraging::Compound, QuantLib
//             defaults for endOfMonth/fixedPaymentFrequency/fixedCalendar,
//             wire lookback 0 => Null<Natural>() (off), lockout 0, no
//             observation shift, no exogenous discount curve.
//   * index:  OvernightIndex("SOFR", 0 fixing days, USD,
//             UnitedStates(GovernmentBond), Actual360) as
//             src/market/index_registry_builder.h builds it; for pricing the
//             index is tied to the bootstrapped curve (the engine clones it
//             with the forwarding handle — src/market/index_registry.h).
//   * swap:   both legs on one Schedule(2025-01-17, +tenor, Annual,
//             UnitedStates(GovernmentBond), ModifiedFollowing,
//             ModifiedFollowing, DateGeneration::Forward, no EOM), then
//             OvernightIndexedSwap with fixed day counter Actual360, spread 0,
//             payment lag 2, payment convention ModifiedFollowing, payment
//             calendar UnitedStates(GovernmentBond), telescopic value dates
//             off, Compound averaging, lookback off, lockout 0, no observation
//             shift, priced with DiscountingSwapEngine on the same curve
//             (src/mappers/ois_swap_mapper.cpp + src/evaluators/ois_swap_evaluator.cpp).
//
// This file is a validation artifact: it is intentionally NOT part of the
// repo's CMake build or test gate. Compile it inside the quantraserver:test
// image (or any environment with the same QuantLib):
//
//   g++ -O2 -std=c++17 native_var_check.cpp \
//       -I/opt/quantra-deps/include -L/opt/quantra-deps/lib -lQuantLib \
//       -o native_var_check
//   LD_LIBRARY_PATH=/opt/quantra-deps/lib \
//       ./native_var_check swaps.csv quotes.csv native_results.csv

#include <ql/currencies/america.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/instruments/overnightindexedswap.hpp>
#include <ql/optional.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/time/calendars/unitedstates.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/utilities/null.hpp>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace QuantLib;

namespace {

// Fixed dates of the example (AS_OF_DATE / SPOT_DATE in historical_var.py).
const Date kAsOfDate(15, January, 2025);
const Date kSpotDate(17, January, 2025);

struct SwapSpec {
    int tenorYears;
    bool isPayer;
    double notional;
    double fixedRate;
};

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> cells;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ','))
        cells.push_back(cell);
    return cells;
}

std::vector<SwapSpec> readSwaps(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    std::vector<SwapSpec> book;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        if (header) { // "tenor_years,is_payer,notional,fixed_rate"
            header = false;
            continue;
        }
        auto cells = splitCsv(line);
        if (cells.size() != 4)
            throw std::runtime_error(path + ": expected 4 columns, got line: " + line);
        SwapSpec s;
        s.tenorYears = std::stoi(cells[0]);
        s.isPayer = std::stoi(cells[1]) != 0;
        s.notional = std::stod(cells[2]);
        s.fixedRate = std::stod(cells[3]);
        book.push_back(s);
    }
    if (book.empty())
        throw std::runtime_error(path + ": no swap rows");
    return book;
}

// Parse a pillar label like "1M" / "30Y" into a QuantLib Period.
Period parseTenorLabel(const std::string& label) {
    if (label.size() < 2)
        throw std::runtime_error("bad pillar tenor label: '" + label + "'");
    const char unit = label.back();
    const int n = std::stoi(label.substr(0, label.size() - 1));
    if (unit == 'M')
        return Period(n, Months);
    if (unit == 'Y')
        return Period(n, Years);
    throw std::runtime_error("bad pillar tenor label: '" + label + "'");
}

// quotes.csv: header row = pillar tenor labels; first data row = base par
// quotes; then one row per scenario with the shocked quotes.
void readQuotes(const std::string& path,
                std::vector<Period>& tenors,
                std::vector<std::vector<double>>& rows) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        auto cells = splitCsv(line);
        if (header) {
            for (const auto& c : cells)
                tenors.push_back(parseTenorLabel(c));
            header = false;
            continue;
        }
        if (cells.size() != tenors.size())
            throw std::runtime_error(path + ": quote row width != header width");
        std::vector<double> row;
        row.reserve(cells.size());
        for (const auto& c : cells)
            row.push_back(std::stod(c));
        rows.push_back(std::move(row));
    }
    if (rows.size() < 2)
        throw std::runtime_error(path + ": need a base row plus >=1 scenario row");
}

// Bootstrap one curve from the par quote vector and price the whole book on
// it. Returns the book value (sum of swap NPVs).
double priceBook(const std::vector<SwapSpec>& book,
                 const std::vector<Period>& pillarTenors,
                 const std::vector<double>& quotes) {
    const Calendar cal = UnitedStates(UnitedStates::GovernmentBond);

    // The index as the engine's IndexRegistry builds it (no forwarding curve;
    // OISRateHelper internally ties forwarding to the curve being built).
    auto bootstrapIndex = ext::make_shared<OvernightIndex>(
        "SOFR", 0, USDCurrency(), cal, Actual360());

    std::vector<ext::shared_ptr<RateHelper>> helpers;
    helpers.reserve(pillarTenors.size());
    for (Size i = 0; i < pillarTenors.size(); ++i) {
        Handle<Quote> q(ext::make_shared<SimpleQuote>(quotes[i]));
        // Argument-for-argument the OISRateHelper call in
        // src/parsers/term_structure_point_parser.cpp for the script's JSON.
        helpers.push_back(ext::make_shared<OISRateHelper>(
            2,                          // settlement_days
            pillarTenors[i],            // tenor
            q,                          // par quote
            bootstrapIndex,             // overnight index
            Handle<YieldTermStructure>(), // no exogenous discount curve
            false,                      // telescopicValueDates
            2,                          // payment_lag
            ModifiedFollowing,          // fixed_leg_convention -> paymentConvention
            Annual,                     // fixed_leg_frequency -> paymentFrequency
            cal,                        // calendar -> paymentCalendar
            0 * Days,                   // forwardStart (spot-starting)
            0.0,                        // overnightSpread
            Pillar::LastRelevantDate,
            Date(),                     // customPillarDate (unused)
            RateAveraging::Compound,    // averaging_method
            ext::nullopt,               // endOfMonth: QuantLib default
            ext::nullopt,               // fixedPaymentFrequency
            Calendar(),                 // fixedCalendar: QuantLib default
            Null<Natural>(),            // lookback_days 0 -> off
            0,                          // lockout_days
            false));                    // apply_observation_shift
    }

    auto curve = ext::make_shared<PiecewiseYieldCurve<Discount, LogLinear>>(
        kAsOfDate, helpers, Actual365Fixed(),
        PiecewiseYieldCurve<Discount, LogLinear>::bootstrap_type(1.0e-15));
    Handle<YieldTermStructure> curveHandle(curve);

    // Pricing index: same definition, forwarding off the bootstrapped curve
    // (the engine clones the registry index with this handle).
    auto pricingIndex = ext::make_shared<OvernightIndex>(
        "SOFR", 0, USDCurrency(), cal, Actual360(), curveHandle);
    auto engine = ext::make_shared<DiscountingSwapEngine>(curveHandle);

    double bookValue = 0.0;
    for (const auto& s : book) {
        const Date termination(17, January, 2025 + s.tenorYears);
        // The script's schedule JSON, as src/parsers/schedule_parser.cpp
        // builds it (both legs share the same schedule definition).
        Schedule schedule(kSpotDate, termination, Period(Annual), cal,
                          ModifiedFollowing, ModifiedFollowing,
                          DateGeneration::Forward, false);
        // Argument-for-argument the OvernightIndexedSwap construction in
        // src/evaluators/ois_swap_evaluator.cpp for the script's JSON.
        OvernightIndexedSwap swap(
            s.isPayer ? OvernightIndexedSwap::Payer
                      : OvernightIndexedSwap::Receiver,
            s.notional,
            schedule,                   // fixed leg schedule
            s.fixedRate,
            Actual360(),                // fixed leg day counter
            schedule,                   // overnight leg schedule
            pricingIndex,
            0.0,                        // spread
            2,                          // payment_lag
            ModifiedFollowing,          // payment_convention
            cal,                        // payment_calendar
            false,                      // telescopic_value_dates
            RateAveraging::Compound,    // averaging_method
            Null<Natural>(),            // lookback_days 0 -> off
            0,                          // lockout_days
            false);                     // apply_observation_shift
        swap.setPricingEngine(engine);
        bookValue += swap.NPV();
    }
    return bookValue;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: native_var_check swaps.csv quotes.csv native_results.csv\n"
                  << "  (inputs from: historical_var.py --dump-inputs DIR)\n";
        return 2;
    }
    try {
        Settings::instance().evaluationDate() = kAsOfDate;

        const auto book = readSwaps(argv[1]);
        std::vector<Period> pillarTenors;
        std::vector<std::vector<double>> quoteRows;
        readQuotes(argv[2], pillarTenors, quoteRows);

        std::ofstream out(argv[3]);
        if (!out)
            throw std::runtime_error(std::string("cannot open ") + argv[3]);
        out << std::setprecision(std::numeric_limits<double>::max_digits10);

        const double baseValue = priceBook(book, pillarTenors, quoteRows[0]);
        out << baseValue << "\n";
        for (Size i = 1; i < quoteRows.size(); ++i) {
            const double scenarioValue = priceBook(book, pillarTenors, quoteRows[i]);
            out << (scenarioValue - baseValue) << "\n";
        }

        std::cout << "native_var_check: " << book.size() << " swaps, "
                  << (quoteRows.size() - 1) << " scenarios, base book value "
                  << std::setprecision(std::numeric_limits<double>::max_digits10)
                  << baseValue << " -> " << argv[3] << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "native_var_check ERROR: " << e.what() << std::endl;
        return 1;
    }
}
