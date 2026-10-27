#ifndef QUANTRA_STRUCTURED_NOTE_DATA_H
#define QUANTRA_STRUCTURED_NOTE_DATA_H

/**
 * Plain (FlatBuffers-free) description of one `StructuredNote` and of its
 * pricing result. The mapper (structured_note_mapper.h) lifts the wire tables
 * into these structs after every schema rule that needs no market data has
 * been checked; the evaluator (structured_note_evaluator.h) resolves the ids
 * against the PricingRegistry, resolves the past, simulates the future and
 * fills the result structs; the mapper serialises them back. Mirrors
 * flatbuffers/fbs/structured_note.fbs and structured_note_response.fbs field
 * by field so the response builder is a straight copy.
 */

#include "enum_convert.h"               // quantra::enums::VolSurfaceShape for the model echo
#include "implied_forwards_evaluator.h" // OptionQuoteLine, QuoteFilterData, DroppedQuoteData

#include <ql/time/date.hpp>

#include <optional>
#include <string>
#include <vector>

namespace quantra {

// ---------------------------------------------------------------------------
// Trade
// ---------------------------------------------------------------------------

enum class BasketPerformanceRule { WorstOf, BestOf, Average };
enum class CapitalBarrierMonitoring { European, DailyClose };
enum class RandomSequenceKind { Sobol, PseudoRandom };
enum class StructuredNoteStatus { Alive, Called, Matured };

/// A close on a past observation date (`BasketUnderlying.historical_levels`).
struct HistoricalLevel {
    QuantLib::Date date;
    double value = 0.0;
};

struct QuantoData {
    std::string discountingCurveId;
    std::string fxSpotId;
    std::string fxVolatilityId;
    double fxUnderlyingCorrelation = 0.0;
};

struct BasketUnderlyingData {
    std::string underlyingId;
    std::string volatilityId;
    double initialLevel = 0.0;
    std::optional<QuantoData> quanto;
    std::vector<HistoricalLevel> historicalLevels;
};

struct ObservationEventData {
    QuantLib::Date observationDate;
    /// Present iff a trigger is present or the event is `isFinal` (the mapper
    /// enforces it; for an `isFinal` event it equals the maturity payment
    /// date).
    std::optional<QuantLib::Date> paymentDate;
    std::optional<double> autocallTrigger;
    std::optional<double> couponTrigger;
    std::optional<double> coupon;
    /// `is_final`: a maturity-only observation. Only the capital protection
    /// block (and the upside) is tested; no coupon test, no call test, the
    /// memory is not paid. The mapper guarantees no trigger, no coupon, and
    /// that it is the last event.
    bool isFinal = false;

    /// Neither trigger and not the final marker: nothing is observed.
    bool accrualOnly() const { return !isFinal && !autocallTrigger && !couponTrigger; }
    /// A coupon test of its own, or (autocall only) the call test; an
    /// `isFinal` event has none.
    bool hasCouponTest() const { return !accrualOnly() && !isFinal; }
    double couponOrZero() const { return coupon ? *coupon : 0.0; }
};

struct CouponRuleData {
    bool memory = false;
    bool payOnlyOnCall = false;
};

struct CapitalProtectionData {
    double barrier = 0.0;
    CapitalBarrierMonitoring monitoring = CapitalBarrierMonitoring::European;
    double putStrike = 1.0;
    double participation = 1.0;
    std::optional<bool> knockedIn;
};

struct UpsideData {
    double strike = 0.0;
    double participation = 0.0;
    std::optional<double> cap;
};

struct MonteCarloData {
    unsigned int paths = 0;
    unsigned long seed = 42;
    RandomSequenceKind sequence = RandomSequenceKind::Sobol;
    bool antithetic = true;
    bool brownianBridge = true;
    std::optional<unsigned int> stepsPerYear;
};

struct StructuredNoteTrade {
    std::string tradeId;
    std::string currency;
    double notional = 0.0;
    std::vector<BasketUnderlyingData> underlyings;
    BasketPerformanceRule performanceRule = BasketPerformanceRule::WorstOf;
    std::vector<ObservationEventData> events;
    CouponRuleData couponRule;
    CapitalProtectionData capital;
    QuantLib::Date maturityPaymentDate;
    std::string discountingCurveId;
    double fundingSpreadBp = 0.0;
    std::string correlationId;
    /// Uniform shift of every off-diagonal correlation before pricing;
    /// absent = none.
    std::optional<double> correlationMarkup;
    std::optional<UpsideData> upside;
    MonteCarloData monteCarlo;
    /// `notes[<i>]`, the prefix of every message about this note.
    std::string path;
};

/// One `diagnostics.vanilla_repricing_sheets[]` entry as decoded.
struct NoteRepricingSheet {
    std::string sheetId;
    std::string underlyingId;
    OptionSheetStyle style = OptionSheetStyle::European;
    std::vector<OptionQuoteLine> quotes;
    /// `diagnostics.vanilla_repricing_sheets[<i>]`.
    std::string path;
};

/// `PriceStructuredNoteRequest.diagnostics` (every flag false = no block).
struct NoteDiagnosticsRequest {
    bool market = false;
    bool decomposition = false;
    bool checks = false;
    bool distribution = false;
    bool timing = false;
    /// Individual paths returned with `distribution` (mapper: <= 100).
    unsigned int samplePaths = 0;
    /// Vanilla repricing through the Monte Carlo (mapper: iff sheets).
    bool vanillaRepricing = false;
    std::vector<NoteRepricingSheet> repricingSheets;
    QuoteFilterData repricingFilter;

    bool any() const {
        return market || decomposition || checks || distribution || timing || vanillaRepricing;
    }
};

/// `PriceStructuredNoteRequest.greeks` (bump sizes; the mapper checks each is
/// finite and > 0). Present = Greeks requested.
struct NoteGreeksRequest {
    double spotBump = 0.01;
    double volBump = 0.01;
    double correlationBump = 0.01;
    double fundingBumpBp = 1.0;
    double rateBumpBp = 1.0;
};

struct StructuredNoteInputs {
    std::vector<StructuredNoteTrade> notes;
    NoteDiagnosticsRequest diagnostics;
    std::optional<NoteGreeksRequest> greeks;
};

// ---------------------------------------------------------------------------
// Result (mirrors structured_note_response.fbs)
// ---------------------------------------------------------------------------

struct NoteTermsUnderlying {
    std::string underlyingId;
    std::string currency;
    double initialLevel = 0.0;
    std::string volatilityId;
    bool quanto = false;
    std::string fxSpotId;
    std::string fxVolatilityId;
    std::optional<double> fxUnderlyingCorrelation;
};

struct NoteTermsEvent {
    unsigned int index = 0;
    std::string observationDate;
    std::optional<std::string> paymentDate;
    std::optional<double> autocallTrigger;
    std::optional<double> couponTrigger;
    std::optional<double> coupon;
    bool accrualOnly = false;
    bool final = false;
    /// The request's `is_final` marker (maturity-only observation).
    bool isFinal = false;
    bool resolvedFromHistory = false;
};

struct NoteTermsEcho {
    std::string tradeId;
    std::string currency;
    double notional = 0.0;
    BasketPerformanceRule performanceRule = BasketPerformanceRule::WorstOf;
    std::vector<NoteTermsUnderlying> underlyings;
    std::vector<NoteTermsEvent> events;
    bool couponMemory = false;
    bool couponPayOnlyOnCall = false;
    double capitalBarrier = 0.0;
    CapitalBarrierMonitoring capitalMonitoring = CapitalBarrierMonitoring::European;
    double putStrike = 1.0;
    double participation = 1.0;
    std::string maturityPaymentDate;
    std::string discountingCurveId;
    double fundingSpreadBp = 0.0;
    std::string correlationId;
    std::optional<double> correlationMarkup;
    bool correlationMarkupRepaired = false;
    unsigned int paths = 0;
    unsigned long seed = 42;
    RandomSequenceKind sequence = RandomSequenceKind::Sobol;
    bool antithetic = true;
    bool brownianBridge = true;
    std::optional<unsigned int> stepsPerYear;
};

struct NoteResolvedEvent {
    unsigned int index = 0;
    std::string observationDate;
    bool accrualOnly = false;
    std::vector<double> levels;
    std::vector<double> performances;
    std::optional<double> basketPerformance;
    std::optional<bool> called;
    std::optional<bool> couponEarned;
    double couponPaid = 0.0;
    double memoryAfter = 0.0;
};

struct NoteSeasoning {
    std::string asOfDate;
    StructuredNoteStatus status = StructuredNoteStatus::Alive;
    std::vector<NoteResolvedEvent> events;
    double memoryCarried = 0.0;
    std::optional<unsigned int> nextEventIndex;
    std::optional<std::string> knownPaymentDate;
    std::optional<double> knownPayment;
};

struct NoteModelUnderlying {
    std::string underlyingId;
    quantra::enums::VolSurfaceShape volShape = quantra::enums::VolSurfaceShape_Constant;
    bool localVol = false;
    std::optional<double> quantoDrift;
};

struct NoteModelEcho {
    std::vector<NoteModelUnderlying> underlyings;
    bool observationDatesOnly = true;
    unsigned int timeSteps = 0;
    std::vector<double> timeGrid;
    unsigned int sequenceDimension = 0;
};

struct NoteBucket {
    double pv = 0.0;
    double standardError = 0.0;
};

struct NoteLegs {
    NoteBucket bondRedemption;
    NoteBucket coupons;
    NoteBucket capitalLoss;
};

struct NoteEventProbability {
    unsigned int index = 0;
    std::string observationDate;
    std::optional<double> pCall;
    std::optional<double> pCoupon;
    /// Standard errors of the two probabilities (tests; not on the wire).
    double pCallStandardError = 0.0;
    double pCouponStandardError = 0.0;
};

// --- opt-in Greeks (definitions: the fbs doc comments) ---

/// A sensitivity with the standard error of its per-path difference.
struct NoteSensitivity {
    double value = 0.0;
    double standardError = 0.0;
};

struct NoteUnderlyingGreeks {
    std::string underlyingId;
    NoteSensitivity delta;
    NoteSensitivity gamma;
    NoteSensitivity vega;
};

struct NoteGreeks {
    std::vector<NoteUnderlyingGreeks> underlyings;
    /// Absent for a single underlying.
    std::optional<NoteSensitivity> correlationSensitivity;
    bool correlationBumpRepaired = false;
    /// Row-major n x n, the matrix the bumped run used; empty for a single
    /// underlying.
    std::vector<double> correlationBumped;
    NoteSensitivity fundingDv01;
    NoteSensitivity rho;
    double spotBump = 0.0;
    double volBump = 0.0;
    double correlationBump = 0.0;
    double fundingBumpBp = 0.0;
    double rateBumpBp = 0.0;
};

// --- opt-in diagnostics (definitions: the fbs doc comments) ---

struct NoteMarketUnderlying {
    std::string underlyingId;
    double spot = 0.0;
    std::vector<double> forwards;
    std::vector<double> impliedDividendYields;
    std::vector<double> impliedRates;
    std::vector<double> atmVols;
    std::vector<double> barrierVols;
    std::optional<double> quantoDriftBp;
};

struct NoteMarketDiagnostics {
    std::vector<std::string> observationDates;
    std::vector<NoteMarketUnderlying> underlyings;
    std::vector<std::string> paymentDates;
    std::vector<double> discountFactors;
    std::vector<double> discountFactorsWithFunding;
    std::vector<std::string> correlationLabels;
    /// Row-major n x n: the matrix the simulation ran on.
    std::vector<double> correlation;
    /// With a correlation mark-up: the matrix as supplied (same order);
    /// empty otherwise.
    std::vector<double> correlationBase;
    std::optional<double> correlationMarkup;
};

struct NoteEventDecomposition {
    unsigned int index = 0;
    std::string observationDate;
    std::optional<double> pCall;
    std::optional<double> pCoupon;
    double pvCoupons = 0.0;
    double pvRedemption = 0.0;
};

struct NoteDecompositionDiagnostics {
    std::vector<NoteEventDecomposition> events;
    double pSurviveToMaturity = 0.0;
    double pBreach = 0.0;
    double expectedLifeYears = 0.0;
    std::optional<double> expectedLossGivenBreach;
};

struct NoteRunningMean {
    double fraction = 0.0;
    unsigned int paths = 0;
    double npv = 0.0;
    double standardError = 0.0;
};

struct NoteChecksDiagnostics {
    std::vector<NoteRunningMean> runningMeans;
    double deterministicForwardNpv = 0.0;
    double correlationOneNpv = 0.0;
    double correlationOneStandardError = 0.0;
    double correlationZeroNpv = 0.0;
    double correlationZeroStandardError = 0.0;
    double bondOnlyNpv = 0.0;
};

struct NoteQuantiles {
    std::string observationDate;
    double q05 = 0.0, q25 = 0.0, q50 = 0.0, q75 = 0.0, q95 = 0.0;
};

struct NoteHistogramBin {
    double lower = 0.0;
    double upper = 0.0;
    unsigned int paths = 0;
};

struct NoteSamplePath {
    std::vector<double> basketPerformances;
    double payout = 0.0;
    double payoutPv = 0.0;
    std::optional<std::string> calledOn;
    bool breached = false;
};

struct NoteDistributionDiagnostics {
    std::vector<NoteQuantiles> basketPerformanceQuantiles;
    double payoutQ05 = 0.0, payoutQ25 = 0.0, payoutQ50 = 0.0, payoutQ75 = 0.0, payoutQ95 = 0.0;
    std::vector<NoteHistogramBin> payoutHistogram;
    std::vector<NoteSamplePath> samplePaths;
};

struct NoteTimingStage {
    std::string stage;
    double milliseconds = 0.0;
};

struct NoteTimingDiagnostics {
    std::vector<NoteTimingStage> stages;
    double totalMilliseconds = 0.0;
};

/// `StructuredNoteRepricedOption`.
struct NoteRepricedOption {
    std::string expiry;
    double time = 0.0;
    double strike = 0.0;
    bool isCall = true;
    double bid = 0.0, ask = 0.0, mid = 0.0;
    double modelPrice = 0.0;
    double standardError = 0.0;
    bool insideBidAsk = false;
    double priceError = 0.0;
    std::optional<double> marketVol;
    std::optional<double> modelVol;
    std::optional<double> volError;
};

/// `StructuredNoteRepricingExpiry`.
struct NoteRepricingExpiry {
    std::string expiry;
    double time = 0.0;
    double forward = 0.0;
    double discountFactor = 0.0;
    unsigned int quotesUsed = 0;
    std::vector<DroppedQuoteData> dropped;
};

/// `StructuredNoteVanillaRepricingUnderlying`.
struct NoteRepricingUnderlying {
    std::string underlyingId;
    std::string sheetId;
    double spot = 0.0;
    std::vector<NoteRepricingExpiry> expiries;
    unsigned int count = 0;
    unsigned int insideBidAsk = 0;
    double maxAbsPriceError = 0.0;
    std::optional<double> maxAbsVolError;
    double maxStandardError = 0.0;
    std::vector<NoteRepricedOption> options;
};

/// `StructuredNoteVanillaRepricing`.
struct NoteVanillaRepricing {
    unsigned int paths = 0;
    unsigned int timeSteps = 0;
    double noteNpv = 0.0;
    double noteStandardError = 0.0;
    unsigned int count = 0;
    unsigned int insideBidAsk = 0;
    double maxAbsPriceError = 0.0;
    std::optional<double> maxAbsVolError;
    double maxStandardError = 0.0;
    std::vector<NoteRepricingUnderlying> underlyings;
};

struct NoteResultRow {
    std::string tradeId;
    double npv = 0.0;
    double npvPctOfNotional = 0.0;
    std::optional<double> standardError;
    unsigned int paths = 0;
    unsigned long seed = 42;
    StructuredNoteStatus status = StructuredNoteStatus::Alive;
    std::optional<NoteLegs> legs;
    std::vector<NoteEventProbability> eventProbabilities;
    std::optional<double> pBarrierBreach;
    std::optional<double> expectedLifeYears;
    /// Present only when requested (a request for Greeks on a note that is
    /// not alive is a 400).
    std::optional<NoteGreeks> greeks;
    NoteTermsEcho terms;
    NoteSeasoning seasoning;
    NoteModelEcho model;
    /// Present only when requested and something was simulated.
    std::optional<NoteMarketDiagnostics> market;
    std::optional<NoteDecompositionDiagnostics> decomposition;
    std::optional<NoteChecksDiagnostics> checks;
    std::optional<NoteDistributionDiagnostics> distribution;
    std::optional<NoteTimingDiagnostics> timing;
    std::optional<NoteVanillaRepricing> vanillaRepricing;
};

struct NoteResult {
    std::vector<NoteResultRow> notes;
};

} // namespace quantra

#endif // QUANTRA_STRUCTURED_NOTE_DATA_H
