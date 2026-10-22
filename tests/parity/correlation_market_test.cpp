// Correlation market data (CorrelationMatrixSpec -> CorrelationRegistry) and
// the quanto dividend-curve builder: contract tests.
//
// Correlation: the parser rejects every structural defect with a 400 that
// names the matrix position, the rule and the offending labels (shape, label
// count, duplicate label, |rho| > 1, diagonal != 1, asymmetry); the registry
// builder rejects a non-PSD matrix naming the smallest eigenvalue and a
// duplicate id; a rank-deficient matrix (rho = 1 on every pair, eigenvalue 0)
// is accepted; the sub-matrix lookup returns the matrix permuted to the
// caller's label order and names a label it does not have.
//
// Quanto: with flat curves and vols, a Black-Scholes process on the note
// curve whose dividend curve is the quanto curve has forward
// S * exp((r_local - q - rho * sigma_S * sigma_FX) * T) (QuantLib
// QuantoTermStructure wired as its QuantoEngine wires it) to 1e-12 relative,
// and rho = 0 gives the plain forward in the underlying's own currency.
#include "correlation_parser.h"
#include "correlation_registry.h"
#include "error.h"
#include "quanto_builder.h"

#include "correlation_generated.h"
#include "volatility_generated.h"

#include <ql/math/matrix.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace quantra { namespace testing {

namespace {

// Four-index basket in the order of the BNP factsheet; a PSD matrix with
// distinct off-diagonal entries so a permutation is visible.
const std::vector<std::string> kQuadLabels = {"SMI", "SPX", "SX5E", "NKY"};
const std::vector<double> kQuadValues = {
    1.00, 0.55, 0.80, 0.45, //
    0.55, 1.00, 0.60, 0.50, //
    0.80, 0.60, 1.00, 0.50, //
    0.45, 0.50, 0.50, 1.00, //
};

struct SpecBuffer {
    flatbuffers::FlatBufferBuilder builder;
    const quantra::CorrelationMatrixSpec* spec = nullptr;
};

// Build one CorrelationMatrixSpec; `nRows`/`nCols` default to the label count.
std::unique_ptr<SpecBuffer> makeSpec(
    const std::string& id,
    const std::vector<std::string>& labels,
    const std::vector<double>& values,
    int nRows = -1,
    int nCols = -1) {
    auto out = std::make_unique<SpecBuffer>();
    auto& b = out->builder;
    std::vector<flatbuffers::Offset<flatbuffers::String>> labelOffsets;
    for (const auto& l : labels)
        labelOffsets.push_back(b.CreateString(l));
    const int n = static_cast<int>(labels.size());
    auto matrix = quantra::CreateQuoteMatrix2DDirect(
        b, nRows < 0 ? n : nRows, nCols < 0 ? n : nCols, &values);
    auto spec = quantra::CreateCorrelationMatrixSpecDirect(b, id.c_str(), &labelOffsets, matrix);
    b.Finish(spec);
    out->spec = flatbuffers::GetRoot<quantra::CorrelationMatrixSpec>(b.GetBufferPointer());
    return out;
}

CorrelationMatrix parseDomain(
    const std::string& id,
    const std::vector<std::string>& labels,
    const std::vector<double>& values,
    int nRows = -1,
    int nCols = -1) {
    auto buf = makeSpec(id, labels, values, nRows, nCols);
    QuoteRegistry quotes;
    return CorrelationParser().parse(buf->spec, &quotes, 0);
}

// Run `fn`, require a QuantraInvalidArgument whose message carries every
// substring, and return the message.
std::string expectInvalid(
    const std::function<void()>& fn, const std::vector<std::string>& substrings) {
    try {
        fn();
    } catch (const QuantraInvalidArgument& e) {
        const std::string message = e.what();
        for (const auto& s : substrings) {
            EXPECT_NE(message.find(s), std::string::npos)
                << "message '" << message << "' lacks '" << s << "'";
        }
        return message;
    } catch (const std::exception& e) {
        ADD_FAILURE() << "expected QuantraInvalidArgument, got: " << e.what();
        return "";
    }
    ADD_FAILURE() << "expected QuantraInvalidArgument, nothing thrown";
    return "";
}

std::vector<double> allOnes(std::size_t n) {
    return std::vector<double>(n * n, 1.0);
}

} // namespace

// ---------------------------------------------------------------------------
// Parser + registry: accepted matrices
// ---------------------------------------------------------------------------

TEST(CorrelationMarket, ValidFourByFourIsAccepted) {
    const CorrelationMatrix domain = parseDomain("QUAD", kQuadLabels, kQuadValues);
    EXPECT_EQ(domain.id, "QUAD");
    EXPECT_EQ(domain.n, 4u);
    EXPECT_EQ(domain.labels, kQuadLabels);
    EXPECT_EQ(domain.values, kQuadValues);

    const CorrelationMatrixRuntime runtime = MakeCorrelationRuntime(domain);
    EXPECT_EQ(runtime.matrix.rows(), 4u);
    EXPECT_EQ(runtime.matrix.columns(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(runtime.labelIndex.at(kQuadLabels[i]), i);
        for (std::size_t j = 0; j < 4; ++j) {
            EXPECT_DOUBLE_EQ(runtime.matrix[i][j], kQuadValues[i * 4 + j]);
        }
    }
    EXPECT_GT(runtime.smallestEigenvalue, 0.0);
}

TEST(CorrelationMarket, PerfectCorrelationIsRankDeficientAndAccepted) {
    // rho = 1 on every pair: eigenvalues {4, 0, 0, 0}. Accepted (PSD, not PD).
    const CorrelationMatrixRuntime runtime =
        MakeCorrelationRuntime(parseDomain("ONES", kQuadLabels, allOnes(4)));
    EXPECT_NEAR(runtime.smallestEigenvalue, 0.0, 1e-12);
    EXPECT_GE(runtime.smallestEigenvalue, -1e-10);
}

// ---------------------------------------------------------------------------
// Registry builder: PSD check and duplicate ids
// ---------------------------------------------------------------------------

TEST(CorrelationMarket, NotPositiveSemiDefiniteIsRejectedNamingTheEigenvalue) {
    // A at 0.9 with both B and C while B and C are at -0.9 is impossible:
    // smallest eigenvalue -0.8 (det = -2.888).
    const std::vector<double> values = {
        1.0, 0.9,  0.9,  //
        0.9, 1.0,  -0.9, //
        0.9, -0.9, 1.0,  //
    };
    const CorrelationMatrix domain = parseDomain("IMPOSSIBLE", {"A", "B", "C"}, values);
    const std::string message = expectInvalid(
        [&] { MakeCorrelationRuntime(domain); },
        {"correlations[IMPOSSIBLE]", "not positive semi-definite", "smallest eigenvalue -0.8"});
    EXPECT_NE(message.find("-0.8"), std::string::npos) << message;
}

TEST(CorrelationMarket, DuplicateIdAcrossMatricesIsRejected) {
    flatbuffers::FlatBufferBuilder b;
    std::vector<flatbuffers::Offset<quantra::CorrelationMatrixSpec>> matrices;
    for (int k = 0; k < 2; ++k) {
        std::vector<flatbuffers::Offset<flatbuffers::String>> labels;
        for (const auto& l : kQuadLabels)
            labels.push_back(b.CreateString(l));
        auto matrix = quantra::CreateQuoteMatrix2DDirect(b, 4, 4, &kQuadValues);
        matrices.push_back(quantra::CreateCorrelationMatrixSpecDirect(b, "QUAD", &labels, matrix));
    }
    b.Finish(quantra::CreateCorrelationMarketDataDirect(b, &matrices));
    const auto* market = flatbuffers::GetRoot<quantra::CorrelationMarketData>(b.GetBufferPointer());

    QuoteRegistry quotes;
    expectInvalid(
        [&] { CorrelationRegistryBuilder().build(market, quotes); },
        {"duplicate correlation matrix id in pricing.correlations.matrices: QUAD"});
}

TEST(CorrelationMarket, NullMarketDataYieldsEmptyRegistry) {
    QuoteRegistry quotes;
    const CorrelationRegistry reg = CorrelationRegistryBuilder().build(nullptr, quotes);
    EXPECT_EQ(reg.size(), 0u);
    EXPECT_FALSE(reg.has("QUAD"));
    EXPECT_THROW(reg.get("QUAD"), QuantraNotFound);
}

// ---------------------------------------------------------------------------
// Parser: every structural rule is a 400 naming the rule and the labels
// ---------------------------------------------------------------------------

TEST(CorrelationMarket, AsymmetricIsRejectedNamingThePair) {
    std::vector<double> values = kQuadValues;
    values[1 * 4 + 2] = 0.61; // (SPX, SX5E) != (SX5E, SPX) = 0.60
    expectInvalid(
        [&] { parseDomain("QUAD", kQuadLabels, values); },
        {"correlations[0].values: not symmetric", "(SPX, SX5E) = 0.61", "(SX5E, SPX) = 0.6",
         "(matrix 'QUAD')"});
}

TEST(CorrelationMarket, AsymmetryWithinToleranceIsAccepted) {
    std::vector<double> values = kQuadValues;
    values[1 * 4 + 2] = 0.60 + 5e-13;
    const CorrelationMatrixRuntime runtime =
        MakeCorrelationRuntime(parseDomain("QUAD", kQuadLabels, values));
    EXPECT_DOUBLE_EQ(runtime.matrix[1][2], runtime.matrix[2][1]);
}

TEST(CorrelationMarket, DiagonalNotOneIsRejected) {
    std::vector<double> values = kQuadValues;
    values[2 * 4 + 2] = 0.99;
    expectInvalid(
        [&] { parseDomain("QUAD", kQuadLabels, values); },
        {"correlations[0].values: diagonal entry (SX5E, SX5E) must be 1, got 0.99"});
}

TEST(CorrelationMarket, EntryOutsideUnitIntervalIsRejected) {
    std::vector<double> values = kQuadValues;
    values[0 * 4 + 3] = 1.2;
    values[3 * 4 + 0] = 1.2;
    expectInvalid(
        [&] { parseDomain("QUAD", kQuadLabels, values); },
        {"correlations[0].values: entry (SMI, NKY) must be in [-1, 1], got 1.2"});
    values[0 * 4 + 3] = -1.0000001;
    values[3 * 4 + 0] = -1.0000001;
    expectInvalid(
        [&] { parseDomain("QUAD", kQuadLabels, values); }, {"entry (SMI, NKY) must be in [-1, 1]"});
}

TEST(CorrelationMarket, NonFiniteEntryIsRejected) {
    std::vector<double> values = kQuadValues;
    values[0 * 4 + 1] = std::nan("");
    expectInvalid(
        [&] { parseDomain("QUAD", kQuadLabels, values); },
        {"correlations[0].values: entry (SMI, SPX) must be finite"});
}

TEST(CorrelationMarket, LabelCountMismatchIsRejected) {
    // Three labels, a 4x4 matrix.
    expectInvalid(
        [&] { parseDomain("QUAD", {"SMI", "SPX", "SX5E"}, kQuadValues, 4, 4); },
        {"correlations[0].values: n_rows and n_cols must both equal len(labels) = 3, got 4x4"});
    // Dims claim 4x4 but only 12 values.
    std::vector<double> twelve(kQuadValues.begin(), kQuadValues.begin() + 12);
    expectInvalid(
        [&] { parseDomain("QUAD", kQuadLabels, twelve); },
        {"correlations[0].values: values must hold n_rows*n_cols = 16 entries, got 12"});
}

TEST(CorrelationMarket, DuplicateLabelIsRejected) {
    expectInvalid(
        [&] { parseDomain("QUAD", {"SMI", "SPX", "SMI", "NKY"}, kQuadValues); },
        {"correlations[0].labels: duplicate label 'SMI'", "(matrix 'QUAD')"});
}

TEST(CorrelationMarket, FewerThanTwoLabelsIsRejected) {
    expectInvalid(
        [&] { parseDomain("ONE", {"SMI"}, {1.0}); },
        {"correlations[0].labels must list at least two labels"});
}

TEST(CorrelationMarket, MissingIdIsRejected) {
    flatbuffers::FlatBufferBuilder b;
    std::vector<flatbuffers::Offset<flatbuffers::String>> labels;
    for (const auto& l : kQuadLabels)
        labels.push_back(b.CreateString(l));
    auto matrix = quantra::CreateQuoteMatrix2DDirect(b, 4, 4, &kQuadValues);
    b.Finish(quantra::CreateCorrelationMatrixSpecDirect(b, "", &labels, matrix));
    const auto* spec = flatbuffers::GetRoot<quantra::CorrelationMatrixSpec>(b.GetBufferPointer());
    QuoteRegistry quotes;
    expectInvalid(
        [&] { CorrelationParser().parse(spec, &quotes, 2); }, {"correlations[2].id is required"});
}

TEST(CorrelationMarket, QuoteIdsResolveThroughTheQuoteRegistry) {
    flatbuffers::FlatBufferBuilder b;
    std::vector<flatbuffers::Offset<flatbuffers::String>> labels;
    for (const auto& l : kQuadLabels)
        labels.push_back(b.CreateString(l));
    // Inline values carry a wrong (SMI, SPX) entry that the quote overrides.
    std::vector<double> values = kQuadValues;
    values[0 * 4 + 1] = 0.0;
    values[1 * 4 + 0] = 0.0;
    std::vector<flatbuffers::Offset<flatbuffers::String>> quoteIds(16);
    for (std::size_t k = 0; k < 16; ++k)
        quoteIds[k] = b.CreateString("");
    quoteIds[0 * 4 + 1] = b.CreateString("RHO_SMI_SPX");
    quoteIds[1 * 4 + 0] = b.CreateString("RHO_SMI_SPX");
    auto matrix = quantra::CreateQuoteMatrix2DDirect(b, 4, 4, &values, &quoteIds);
    b.Finish(quantra::CreateCorrelationMatrixSpecDirect(b, "QUAD", &labels, matrix));
    const auto* spec = flatbuffers::GetRoot<quantra::CorrelationMatrixSpec>(b.GetBufferPointer());

    QuoteRegistry quotes;
    quotes.upsert("RHO_SMI_SPX", 0.55);
    const CorrelationMatrix domain = CorrelationParser().parse(spec, &quotes, 0);
    EXPECT_EQ(domain.values, kQuadValues);

    QuoteRegistry empty;
    EXPECT_THROW(CorrelationParser().parse(spec, &empty, 0), QuantraNotFound);
}

// ---------------------------------------------------------------------------
// Sub-matrix lookup
// ---------------------------------------------------------------------------

TEST(CorrelationMarket, SubMatrixInPermutedLabelOrderIsPermuted) {
    CorrelationRegistry reg;
    reg.put(MakeCorrelationRuntime(parseDomain("QUAD", kQuadLabels, kQuadValues)));

    // The Santander order of the same basket.
    const std::vector<std::string> permuted = {"SX5E", "SMI", "NKY", "SPX"};
    const QuantLib::Matrix sub = reg.subMatrix("QUAD", permuted);
    ASSERT_EQ(sub.rows(), 4u);
    ASSERT_EQ(sub.columns(), 4u);
    std::vector<std::size_t> original;
    for (const auto& l : permuted) {
        original.push_back(
            std::find(kQuadLabels.begin(), kQuadLabels.end(), l) - kQuadLabels.begin());
    }
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            EXPECT_DOUBLE_EQ(sub[i][j], kQuadValues[original[i] * 4 + original[j]])
                << permuted[i] << "/" << permuted[j];
        }
    }
    // Spot checks on the permuted entries.
    EXPECT_DOUBLE_EQ(sub[0][1], 0.80); // (SX5E, SMI)
    EXPECT_DOUBLE_EQ(sub[0][3], 0.60); // (SX5E, SPX)
    EXPECT_DOUBLE_EQ(sub[2][3], 0.50); // (NKY, SPX)
    EXPECT_DOUBLE_EQ(sub[1][2], 0.45); // (SMI, NKY)

    // Identity order gives the matrix back unchanged; a subset is the
    // principal sub-matrix.
    const QuantLib::Matrix same = reg.subMatrix("QUAD", kQuadLabels);
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j)
            EXPECT_DOUBLE_EQ(same[i][j], kQuadValues[i * 4 + j]);
    }
    const QuantLib::Matrix pair = reg.subMatrix("QUAD", {"NKY", "SMI"});
    ASSERT_EQ(pair.rows(), 2u);
    EXPECT_DOUBLE_EQ(pair[0][1], 0.45);
    EXPECT_DOUBLE_EQ(pair[1][0], 0.45);
}

TEST(CorrelationMarket, SubMatrixNamesAMissingOrRepeatedLabel) {
    CorrelationRegistry reg;
    reg.put(MakeCorrelationRuntime(parseDomain("QUAD", kQuadLabels, kQuadValues)));
    expectInvalid(
        [&] { reg.subMatrix("QUAD", {"SMI", "DAX"}); },
        {"correlation matrix 'QUAD' has no label 'DAX'", "(labels: SMI, SPX, SX5E, NKY)"});
    expectInvalid(
        [&] { reg.subMatrix("QUAD", {"SMI", "SPX", "SMI"}); },
        {"correlation matrix 'QUAD': label 'SMI' requested twice"});
    EXPECT_THROW(reg.subMatrix("NO_SUCH", kQuadLabels), QuantraNotFound);
}

// ---------------------------------------------------------------------------
// Quanto dividend curve
// ---------------------------------------------------------------------------

namespace {

struct FlatQuantoMarket {
    QuantLib::Date reference{15, QuantLib::January, 2025};
    QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    double spot = 12000.0; // SMI level, also the note's initial level
    double fxSpot = 0.90;  // CHF per USD
    double rLocal = 0.005; // CHF
    double rNote = 0.042;  // USD
    double q = 0.03;
    double sigmaS = 0.18;
    double sigmaFx = 0.08;

    QuantLib::Handle<QuantLib::YieldTermStructure> flat(double rate) const {
        return QuantLib::Handle<QuantLib::YieldTermStructure>(
            std::make_shared<QuantLib::FlatForward>(reference, rate, dc));
    }
    QuantLib::Handle<QuantLib::BlackVolTermStructure> vol(double sigma) const {
        return QuantLib::Handle<QuantLib::BlackVolTermStructure>(
            std::make_shared<QuantLib::BlackConstantVol>(
                reference, QuantLib::NullCalendar(), sigma, dc));
    }
};

} // namespace

TEST(QuantoBuilder, FlatMarketForwardMatchesClosedForm) {
    const FlatQuantoMarket m;
    const auto dividend = m.flat(m.q);
    const auto rLocal = m.flat(m.rLocal);
    const auto rNote = m.flat(m.rNote);
    const auto volS = m.vol(m.sigmaS);
    const auto volFx = m.vol(m.sigmaFx);

    for (const double rho : {-0.5, 0.0, 0.7}) {
        const auto quanto =
            buildQuantoDividendCurve(dividend, rLocal, rNote, volS, volFx, m.fxSpot, rho, m.spot);
        ASSERT_FALSE(quanto.empty());
        EXPECT_EQ(quanto->referenceDate(), m.reference);
        for (const double T : {0.5, 1.0, 3.0, 6.5}) {
            // Forward of a process with riskFreeRate = rNote and dividendYield = quanto.
            const double forward = m.spot * quanto->discount(T) / rNote->discount(T);
            const double expected =
                m.spot * std::exp((m.rLocal - m.q - rho * m.sigmaS * m.sigmaFx) * T);
            EXPECT_NEAR(forward / expected, 1.0, 1e-12) << "rho=" << rho << " T=" << T;
            // The adjustment is visible: a positive rho lowers the forward
            // below the plain local forward, a negative one lifts it.
            const double plain = m.spot * std::exp((m.rLocal - m.q) * T);
            if (rho > 0.0) EXPECT_LT(forward, plain);
            if (rho < 0.0) EXPECT_GT(forward, plain);
        }
    }
}

TEST(QuantoBuilder, ZeroCorrelationIsThePlainLocalForward) {
    const FlatQuantoMarket m;
    const auto dividend = m.flat(m.q);
    const auto rLocal = m.flat(m.rLocal);
    const auto rNote = m.flat(m.rNote);
    const auto quanto = buildQuantoDividendCurve(
        dividend, rLocal, rNote, m.vol(m.sigmaS), m.vol(m.sigmaFx), m.fxSpot, 0.0, m.spot);
    for (const double T : {1.0, 4.0}) {
        const double forward = m.spot * quanto->discount(T) / rNote->discount(T);
        const double plainLocal = m.spot * dividend->discount(T) / rLocal->discount(T);
        const double plainNote = m.spot * dividend->discount(T) / rNote->discount(T);
        EXPECT_NEAR(forward / plainLocal, 1.0, 1e-12);
        // The note-currency rate does not enter the drift.
        EXPECT_GT(std::fabs(forward / plainNote - 1.0), 1e-3);
    }
}

TEST(QuantoBuilder, InputsAreValidated) {
    const FlatQuantoMarket m;
    const auto dividend = m.flat(m.q);
    const auto rLocal = m.flat(m.rLocal);
    const auto rNote = m.flat(m.rNote);
    const auto volS = m.vol(m.sigmaS);
    const auto volFx = m.vol(m.sigmaFx);
    expectInvalid(
        [&] {
            buildQuantoDividendCurve(dividend, rLocal, rNote, volS, volFx, m.fxSpot, 1.5, m.spot);
        },
        {"fx_underlying_correlation must be in [-1, 1]"});
    expectInvalid(
        [&] { buildQuantoDividendCurve(dividend, rLocal, rNote, volS, volFx, 0.0, 0.3, m.spot); },
        {"FX spot must be positive"});
    expectInvalid(
        [&] {
            buildQuantoDividendCurve(dividend, rLocal, rNote, volS, volFx, m.fxSpot, 0.3, -1.0);
        },
        {"strike (initial level) must be positive"});
    const QuantLib::Handle<QuantLib::YieldTermStructure> empty;
    EXPECT_THROW(
        buildQuantoDividendCurve(empty, rLocal, rNote, volS, volFx, m.fxSpot, 0.3, m.spot),
        QuantraError);
}

}} // namespace quantra::testing
