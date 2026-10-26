"""
Quantra vs QuantLib Python Comparison Tests

Tests that Quantra FlatBuffers client produces identical results to raw QuantLib Python.
Uses identical bootstrapped curves for exact matching.

This mirrors the C++ comparison tests in tests/quantra_comparison_test.cpp

Usage:
    PYTHONPATH=/workspace/clients/python:/workspace/flatbuffers/python python3 tests/test_quantra_vs_quantlib.py
"""

import sys
import os

# Get the project root directory (parent of tests/)
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)

# Add paths for imports
sys.path.insert(0, os.path.join(PROJECT_ROOT, 'flatbuffers/python'))
sys.path.insert(0, os.path.join(PROJECT_ROOT, 'clients', 'python'))

import QuantLib as ql
from quantra_client.client import (
    Client,
    # Request types
    PriceFixedRateBondRequestT,
    PriceVanillaSwapRequestT,
    PriceOisSwapRequestT,
    PriceFRARequestT,
    PriceFxForwardRequestT,
    PriceCrossCurrencySwapRequestT,
    PriceCapFloorRequestT,
    PriceSwaptionRequestT,
    PriceCDSRequestT,
    # Building blocks
    PricingT,
    RatesMarketDataT,
    CreditMarketDataT,
    VolatilityMarketDataT,
    EquityMarketDataT,
    PricingOptionsT,
    TermStructureT,
    ScheduleT,
    PointsWrapperT,
    DepositHelperT,
    SwapHelperT,
    YieldT,
    IndexDefT,
    IndexRefT,
    IndexType,
    # NEW: Vol Surface types (replacing VolatilityTermStructureT)
    VolSurfaceSpecT,
    OptionletVolSpecT,
    SwaptionVolSpecT,
    SwaptionVolConstantSpecT,
    SwaptionVolPayload,
    IrVolBaseSpecT,
    VolPayload,
    # NEW: Model types
    ModelSpecT,
    CapFloorModelSpecT,
    SwaptionModelSpecT,
    ModelPayload,
    # Instruments
    FixedRateBondT,
    PriceFixedRateBondT,
    VanillaSwapT,
    FixedCouponsT,
    IborCouponsT,
    PriceVanillaSwapT,
    OisSwapT,
    PriceOisSwapT,
    FRAT,
    PriceFRAT,
    FxSpotSpecT,
    FxMarketDataT,
    FxForwardT,
    PriceFxForwardT,
    OvernightCouponsT,
    SwapCoupons,
    SwapLegT,
    CrossCurrencySwapT,
    PriceCrossCurrencySwapT,
    PayReceive,
    SwapFlowKind,
    CapFloorT,
    PriceCapFloorT,
    SwaptionT,
    PriceSwaptionT,
    CDST,
    PriceCDST,
    CreditCurveSpecT,
    CdsHelperConventionsT,
    CdsQuoteT,
    CdsModelSpecT,
    # Enums
    DayCounter,
    Calendar,
    BusinessDayConvention,
    Frequency,
    PeriodT,
    TimeUnit,
    DateGenerationRule,
    Interpolator,
    BootstrapTrait,
    Compounding,
    RateAveragingType,
    FRAType,
    FxDirection,
    CapFloorType,
    ExerciseType,
    SettlementType,
    ProtectionSide,
    VolatilityType,
    IrModelType,
    CdsHelperModel,
    CdsEngineType,
    Point,
)


# =============================================================================
# Constants matching C++ test
# =============================================================================
FLAT_RATE = 0.03
EVAL_DATE = ql.Date(15, ql.January, 2025)
EVAL_DATE_STR = "2025-01-15"
TOLERANCE = 0.01  # 1 cent tolerance

def mk_period(n: int, unit: int) -> PeriodT:
    p = PeriodT()
    p.n = n
    p.unit = unit
    return p


# =============================================================================
# QuantLib Python Setup
# =============================================================================

def setup_quantlib():
    """Set up QuantLib evaluation date and build curve."""
    ql.Settings.instance().evaluationDate = EVAL_DATE
    return build_quantlib_curve()


def build_quantlib_curve(rate: float = FLAT_RATE):
    """Build the same bootstrapped curve as C++ test (flat at `rate`)."""
    helpers = []
    
    # 3M deposit
    helpers.append(ql.DepositRateHelper(
        rate, ql.Period(3, ql.Months), 2, ql.TARGET(),
        ql.ModifiedFollowing, True, ql.Actual365Fixed()
    ))
    
    # 6M deposit
    helpers.append(ql.DepositRateHelper(
        rate, ql.Period(6, ql.Months), 2, ql.TARGET(),
        ql.ModifiedFollowing, True, ql.Actual365Fixed()
    ))
    
    # 1Y deposit
    helpers.append(ql.DepositRateHelper(
        rate, ql.Period(1, ql.Years), 2, ql.TARGET(),
        ql.ModifiedFollowing, True, ql.Actual365Fixed()
    ))
    
    # 5Y swap
    euribor6m = ql.Euribor6M()
    helpers.append(ql.SwapRateHelper(
        rate, ql.Period(5, ql.Years), ql.TARGET(), ql.Annual,
        ql.ModifiedFollowing, ql.Thirty360(ql.Thirty360.BondBasis), euribor6m
    ))
    
    # 10Y swap
    helpers.append(ql.SwapRateHelper(
        rate, ql.Period(10, ql.Years), ql.TARGET(), ql.Annual,
        ql.ModifiedFollowing, ql.Thirty360(ql.Thirty360.BondBasis), euribor6m
    ))
    
    curve = ql.PiecewiseLogLinearDiscount(EVAL_DATE, helpers, ql.Actual365Fixed())
    curve.enableExtrapolation()
    return ql.YieldTermStructureHandle(curve)


# =============================================================================
# Quantra Helpers
# =============================================================================

def build_quantra_curve(curve_id: str = "discount", rate: float = FLAT_RATE) -> TermStructureT:
    """Build the same curve for Quantra (flat at `rate`)."""
    ts = TermStructureT()
    ts.id = curve_id
    ts.dayCounter = DayCounter.Actual365Fixed
    ts.interpolator = Interpolator.LogLinear
    ts.bootstrapTrait = BootstrapTrait.Discount
    ts.referenceDate = EVAL_DATE_STR
    ts.points = []
    
    # 3M deposit
    dep3m = DepositHelperT()
    dep3m.rate = rate
    dep3m.tenor = mk_period(3, TimeUnit.Months)
    dep3m.fixingDays = 2
    dep3m.calendar = Calendar.TARGET
    dep3m.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    dep3m.dayCounter = DayCounter.Actual365Fixed
    pw = PointsWrapperT()
    pw.pointType = Point.DepositHelper
    pw.point = dep3m
    ts.points.append(pw)
    
    # 6M deposit
    dep6m = DepositHelperT()
    dep6m.rate = rate
    dep6m.tenor = mk_period(6, TimeUnit.Months)
    dep6m.fixingDays = 2
    dep6m.calendar = Calendar.TARGET
    dep6m.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    dep6m.dayCounter = DayCounter.Actual365Fixed
    pw = PointsWrapperT()
    pw.pointType = Point.DepositHelper
    pw.point = dep6m
    ts.points.append(pw)
    
    # 1Y deposit
    dep1y = DepositHelperT()
    dep1y.rate = rate
    dep1y.tenor = mk_period(1, TimeUnit.Years)
    dep1y.fixingDays = 2
    dep1y.calendar = Calendar.TARGET
    dep1y.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    dep1y.dayCounter = DayCounter.Actual365Fixed
    pw = PointsWrapperT()
    pw.pointType = Point.DepositHelper
    pw.point = dep1y
    ts.points.append(pw)
    
    # 5Y swap
    sw5y = SwapHelperT()
    sw5y.rate = rate
    sw5y.tenor = mk_period(5, TimeUnit.Years)
    sw5y.calendar = Calendar.TARGET
    sw5y.swFixedLegFrequency = Frequency.Annual
    sw5y.swFixedLegConvention = BusinessDayConvention.ModifiedFollowing
    sw5y.swFixedLegDayCounter = DayCounter.Thirty360
    float_idx_5y = IndexRefT()
    float_idx_5y.id = "EUR_6M"
    sw5y.floatIndex = float_idx_5y
    sw5y.spread = 0.0
    sw5y.fwdStartDays = 0
    pw = PointsWrapperT()
    pw.pointType = Point.SwapHelper
    pw.point = sw5y
    ts.points.append(pw)
    
    # 10Y swap
    sw10y = SwapHelperT()
    sw10y.rate = rate
    sw10y.tenor = mk_period(10, TimeUnit.Years)
    sw10y.calendar = Calendar.TARGET
    sw10y.swFixedLegFrequency = Frequency.Annual
    sw10y.swFixedLegConvention = BusinessDayConvention.ModifiedFollowing
    sw10y.swFixedLegDayCounter = DayCounter.Thirty360
    float_idx_10y = IndexRefT()
    float_idx_10y.id = "EUR_6M"
    sw10y.floatIndex = float_idx_10y
    sw10y.spread = 0.0
    sw10y.fwdStartDays = 0
    pw = PointsWrapperT()
    pw.pointType = Point.SwapHelper
    pw.point = sw10y
    ts.points.append(pw)
    
    return ts


def build_quantra_index_6m() -> IndexRefT:
    """Build 6M index reference for Quantra."""
    ref = IndexRefT()
    ref.id = "EUR_6M"
    return ref


def build_quantra_index_3m() -> IndexRefT:
    """Build 3M index reference for Quantra."""
    ref = IndexRefT()
    ref.id = "EUR_3M"
    return ref




def build_index_def_eur6m() -> IndexDefT:
    """Build EUR 6M IndexDef (Euribor-like conventions)."""
    idef = IndexDefT()
    idef.id = "EUR_6M"
    idef.name = "Euribor"
    idef.indexType = IndexType.Ibor
    idef.tenor = mk_period(6, TimeUnit.Months)
    idef.fixingDays = 2
    idef.calendar = Calendar.TARGET
    idef.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    idef.dayCounter = DayCounter.Actual360
    idef.endOfMonth = False
    idef.currency = "EUR"
    return idef


def build_index_def_eur3m() -> IndexDefT:
    """Build EUR 3M IndexDef (Euribor-like conventions)."""
    idef = IndexDefT()
    idef.id = "EUR_3M"
    idef.name = "Euribor"
    idef.indexType = IndexType.Ibor
    idef.tenor = mk_period(3, TimeUnit.Months)
    idef.fixingDays = 2
    idef.calendar = Calendar.TARGET
    idef.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    idef.dayCounter = DayCounter.Actual360
    idef.endOfMonth = False
    idef.currency = "EUR"
    return idef


def build_index_def_sofr() -> IndexDefT:
    """Build USD SOFR IndexDef (overnight conventions)."""
    idef = IndexDefT()
    idef.id = "USD_SOFR"
    idef.name = "SOFR"
    idef.indexType = IndexType.Overnight
    idef.tenor = mk_period(0, TimeUnit.Days)
    idef.fixingDays = 0
    idef.calendar = Calendar.UnitedStatesGovernmentBond
    idef.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    idef.dayCounter = DayCounter.Actual360
    idef.endOfMonth = True
    idef.currency = "USD"
    return idef

def build_quantra_pricing(curves: list, vol_surfaces: list = None, models: list = None,
                            indices: list = None, credit_curves: list = None,
                            fx_spots: list = None) -> PricingT:
    """Build Pricing object for Quantra.
    
    Args:
        curves: List of TermStructureT
        vol_surfaces: List of VolSurfaceSpecT (for caps/floors/swaptions)
        models: List of ModelSpecT (for caps/floors/swaptions)
        indices: List of IndexDefT (index definitions referenced by helpers/instruments)
        credit_curves: List of CreditCurveSpecT (CDS credit curves)
        fx_spots: List of FxSpotSpecT (FX spots referenced by FX products)
    """
    p = PricingT()
    p.asOfDate = EVAL_DATE_STR
    p.settlementDate = EVAL_DATE_STR
    rates = RatesMarketDataT()
    rates.indices = indices or []
    rates.curves = curves
    p.rates = rates
    if credit_curves:
        credit = CreditMarketDataT()
        credit.creditCurves = credit_curves
        p.credit = credit
    if vol_surfaces or models:
        volatility = VolatilityMarketDataT()
        volatility.volSurfaces = vol_surfaces or []
        volatility.models = models or []
        p.volatility = volatility
    p.equity = EquityMarketDataT()
    if fx_spots:
        fx = FxMarketDataT()
        fx.spots = fx_spots
        p.fx = fx
    options = PricingOptionsT()
    options.bondPricingDetails = True
    options.bondPricingFlows = False
    p.options = options
    return p


def build_quantra_optionlet_vol(vol_id: str, vol: float, 
                                 vol_type: int = VolatilityType.Lognormal,
                                 displacement: float = 0.0) -> VolSurfaceSpecT:
    """Build optionlet volatility surface for caps/floors.
    
    Args:
        vol_id: Identifier for this vol surface
        vol: Constant volatility value
        vol_type: VolatilityType enum (ShiftedLognormal, Normal, etc.)
        displacement: Displacement for shifted lognormal (0 for pure lognormal)
    """
    # Build the base spec with vol parameters
    base = IrVolBaseSpecT()
    base.referenceDate = EVAL_DATE_STR
    base.calendar = Calendar.TARGET
    base.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    base.dayCounter = DayCounter.Actual365Fixed
    base.volatilityType = vol_type
    base.displacement = displacement
    base.constantVol = vol
    
    # Wrap in OptionletVolSpec
    optionlet = OptionletVolSpecT()
    optionlet.base = base
    
    # Wrap in VolSurfaceSpec with union type
    spec = VolSurfaceSpecT()
    spec.id = vol_id
    spec.payloadType = VolPayload.OptionletVolSpec
    spec.payload = optionlet
    return spec


def build_quantra_swaption_vol(vol_id: str, vol: float,
                                vol_type: int = VolatilityType.Lognormal,
                                displacement: float = 0.0) -> VolSurfaceSpecT:
    """Build swaption volatility surface.
    
    Args:
        vol_id: Identifier for this vol surface
        vol: Constant volatility value
        vol_type: VolatilityType enum
        displacement: Displacement for shifted lognormal
    """
    base = IrVolBaseSpecT()
    base.referenceDate = EVAL_DATE_STR
    base.calendar = Calendar.TARGET
    base.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    base.dayCounter = DayCounter.Actual365Fixed
    base.volatilityType = vol_type
    base.displacement = displacement
    base.constantVol = vol
    
    swaption_const = SwaptionVolConstantSpecT()
    swaption_const.base = base

    swaption_vol = SwaptionVolSpecT()
    swaption_vol.swapIndexId = "EUR_SWAP_6M"
    swaption_vol.payloadType = SwaptionVolPayload.SwaptionVolConstantSpec
    swaption_vol.payload = swaption_const
    
    spec = VolSurfaceSpecT()
    spec.id = vol_id
    spec.payloadType = VolPayload.SwaptionVolSpec
    spec.payload = swaption_vol
    return spec


def build_quantra_cap_floor_model(model_id: str, 
                                   model_type: int = IrModelType.Black) -> ModelSpecT:
    """Build model spec for cap/floor pricing.
    
    Args:
        model_id: Identifier for this model
        model_type: IrModelType enum (Black, ShiftedBlack, Bachelier)
    """
    cap_model = CapFloorModelSpecT()
    cap_model.modelType = model_type
    
    spec = ModelSpecT()
    spec.id = model_id
    spec.payloadType = ModelPayload.CapFloorModelSpec
    spec.payload = cap_model
    return spec


def build_quantra_swaption_model(model_id: str,
                                  model_type: int = IrModelType.Black) -> ModelSpecT:
    """Build model spec for swaption pricing.
    
    Args:
        model_id: Identifier for this model
        model_type: IrModelType enum (Black, ShiftedBlack, Bachelier)
    """
    swaption_model = SwaptionModelSpecT()
    swaption_model.modelType = model_type
    
    spec = ModelSpecT()
    spec.id = model_id
    spec.payloadType = ModelPayload.SwaptionModelSpec
    spec.payload = swaption_model
    return spec


# =============================================================================
# Comparison Tests
# =============================================================================

def test_fixed_rate_bond(client: Client, curve_handle: ql.YieldTermStructureHandle):
    """
    Compare Fixed Rate Bond pricing: QuantLib vs Quantra
    
    Bond: 5Y, 5% coupon, annual, 100 face
    """
    print("\n" + "=" * 60)
    print("FIXED RATE BOND")
    print("=" * 60)
    
    face = 100.0
    coupon = 0.05
    issue = ql.Date(15, ql.January, 2024)
    maturity = ql.Date(15, ql.January, 2029)
    
    # --- QuantLib ---
    schedule = ql.Schedule(
        issue, maturity, ql.Period(ql.Annual), ql.TARGET(),
        ql.Unadjusted, ql.Unadjusted, ql.DateGeneration.Backward, False
    )
    ql_bond = ql.FixedRateBond(
        2, face, schedule, [coupon], ql.ActualActual(ql.ActualActual.ISDA)
    )
    engine = ql.DiscountingBondEngine(curve_handle)
    ql_bond.setPricingEngine(engine)
    ql_npv = ql_bond.NPV()
    
    # --- Quantra ---
    schedule_q = ScheduleT()
    schedule_q.effectiveDate = "2024-01-15"
    schedule_q.terminationDate = "2029-01-15"
    schedule_q.calendar = Calendar.TARGET
    schedule_q.frequency = Frequency.Annual
    schedule_q.convention = BusinessDayConvention.Unadjusted
    schedule_q.terminationDateConvention = BusinessDayConvention.Unadjusted
    schedule_q.dateGenerationRule = DateGenerationRule.Backward
    schedule_q.endOfMonth = False
    
    bond = FixedRateBondT()
    bond.settlementDays = 2
    bond.faceAmount = face
    bond.rate = coupon
    bond.accrualDayCounter = DayCounter.ActualActual
    bond.paymentConvention = BusinessDayConvention.Unadjusted
    bond.redemption = 100.0
    bond.issueDate = "2024-01-15"
    bond.schedule = schedule_q
    
    price_bond = PriceFixedRateBondT()
    price_bond.fixedRateBond = bond
    price_bond.discountingCurve = "discount"
    
    # Add yield specification
    yield_spec = YieldT()
    yield_spec.dayCounter = DayCounter.Actual360
    yield_spec.compounding = Compounding.Compounded
    yield_spec.frequency = Frequency.Annual
    price_bond.yield_ = yield_spec
    
    request = PriceFixedRateBondRequestT()
    request.pricing = build_quantra_pricing([build_quantra_curve("discount")], indices=[build_index_def_eur6m()])
    request.bonds = [price_bond]
    
    response = client.price_fixed_rate_bonds(request)
    q_npv = response.Bonds(0).Npv()
    
    # --- Compare ---
    diff = abs(ql_npv - q_npv)
    print(f"  QuantLib NPV:  {ql_npv:>12.4f}")
    print(f"  Quantra NPV:   {q_npv:>12.4f}")
    print(f"  Difference:    {diff:>12.6f}")
    
    passed = diff < TOLERANCE
    print(f"  Result:        {'✓ PASS' if passed else '✗ FAIL'}")
    return passed, ql_npv, q_npv


def test_vanilla_swap(client: Client, curve_handle: ql.YieldTermStructureHandle):
    """
    Compare Vanilla Swap pricing: QuantLib vs Quantra

    Swap: 5Y payer, 3.5% fixed vs 6M Euribor, 1M notional. The Quantra swap
    is two SwapLeg entries (fixed leg paid, floating leg received, both EUR,
    curves on the legs); the response carries one SwapLegResult per leg,
    legs[0] = fixed, legs[1] = floating, signed from the reported party's
    view, compared against QuantLib's legNPV.
    """
    print("\n" + "=" * 60)
    print("VANILLA SWAP")
    print("=" * 60)
    
    notional = 1_000_000.0
    fixed_rate = 0.035
    start = EVAL_DATE + 2
    end = start + ql.Period(5, ql.Years)
    
    # --- QuantLib ---
    fixed_schedule = ql.Schedule(
        start, end, ql.Period(ql.Annual), ql.TARGET(),
        ql.ModifiedFollowing, ql.ModifiedFollowing, ql.DateGeneration.Forward, False
    )
    float_schedule = ql.Schedule(
        start, end, ql.Period(ql.Semiannual), ql.TARGET(),
        ql.ModifiedFollowing, ql.ModifiedFollowing, ql.DateGeneration.Forward, False
    )
    
    index = ql.Euribor6M(curve_handle)
    
    ql_swap = ql.VanillaSwap(
        ql.VanillaSwap.Payer, notional,
        fixed_schedule, fixed_rate, ql.Thirty360(ql.Thirty360.BondBasis),
        float_schedule, index, 0.0, ql.Actual360()
    )
    engine = ql.DiscountingSwapEngine(curve_handle)
    ql_swap.setPricingEngine(engine)
    ql_npv = ql_swap.NPV()
    ql_fair_rate = ql_swap.fairRate()
    ql_leg_npvs = [ql_swap.legNPV(0), ql_swap.legNPV(1)]   # fixed (paid) < 0, floating > 0

    # --- Quantra ---
    fixed_schedule_q = ScheduleT()
    fixed_schedule_q.effectiveDate = "2025-01-17"
    fixed_schedule_q.terminationDate = "2030-01-17"
    fixed_schedule_q.calendar = Calendar.TARGET
    fixed_schedule_q.frequency = Frequency.Annual
    fixed_schedule_q.convention = BusinessDayConvention.ModifiedFollowing
    fixed_schedule_q.terminationDateConvention = BusinessDayConvention.ModifiedFollowing
    fixed_schedule_q.dateGenerationRule = DateGenerationRule.Forward
    fixed_schedule_q.endOfMonth = False
    
    # The coupon table holds only the kind-specific fields (a fixed rate);
    # notional, schedule and conventions are set on the leg below.
    fixed_coupons = FixedCouponsT()
    fixed_coupons.rate = fixed_rate

    float_schedule_q = ScheduleT()
    float_schedule_q.effectiveDate = "2025-01-17"
    float_schedule_q.terminationDate = "2030-01-17"
    float_schedule_q.calendar = Calendar.TARGET
    float_schedule_q.frequency = Frequency.Semiannual
    float_schedule_q.convention = BusinessDayConvention.ModifiedFollowing
    float_schedule_q.terminationDateConvention = BusinessDayConvention.ModifiedFollowing
    float_schedule_q.dateGenerationRule = DateGenerationRule.Forward
    float_schedule_q.endOfMonth = False

    ibor_coupons = IborCouponsT()
    ibor_coupons.index = build_quantra_index_6m()
    ibor_coupons.spread = 0.0

    # A payer swap: the fixed leg is paid, the floating leg received. Each
    # leg names its currency, its notional / schedule / day count / payment
    # convention and its curves; a fixed leg has no forwarding curve.
    fixed_leg = SwapLegT()
    fixed_leg.payReceive = PayReceive.Pay
    fixed_leg.currency = "EUR"
    fixed_leg.notional = notional
    fixed_leg.schedule = fixed_schedule_q
    fixed_leg.dayCounter = DayCounter.Thirty360
    fixed_leg.paymentConvention = BusinessDayConvention.ModifiedFollowing
    fixed_leg.couponsType = SwapCoupons.FixedCoupons
    fixed_leg.coupons = fixed_coupons
    fixed_leg.discountingCurve = "discount"

    floating_leg = SwapLegT()
    floating_leg.payReceive = PayReceive.Receive
    floating_leg.currency = "EUR"
    floating_leg.notional = notional
    floating_leg.schedule = float_schedule_q
    floating_leg.dayCounter = DayCounter.Actual360
    floating_leg.paymentConvention = BusinessDayConvention.ModifiedFollowing
    floating_leg.couponsType = SwapCoupons.IborCoupons
    floating_leg.coupons = ibor_coupons
    floating_leg.discountingCurve = "discount"
    floating_leg.forwardingCurve = "discount"

    swap = VanillaSwapT()
    swap.fixedLeg = fixed_leg
    swap.floatingLeg = floating_leg

    price_swap = PriceVanillaSwapT()
    price_swap.vanillaSwap = swap

    request = PriceVanillaSwapRequestT()
    request.pricing = build_quantra_pricing([build_quantra_curve("discount")], indices=[build_index_def_eur6m()])
    request.swaps = [price_swap]

    response = client.price_vanilla_swaps(request)
    result = response.Swaps(0)
    q_npv = result.Npv()
    q_fair_rate = result.FairRate()
    q_leg_npvs = [result.Legs(0).Npv(), result.Legs(1).Npv()]

    # --- Compare ---
    npv_diff = abs(ql_npv - q_npv)
    rate_diff = abs(ql_fair_rate - q_fair_rate)
    leg_diff = max(abs(a - b) for a, b in zip(ql_leg_npvs, q_leg_npvs))

    print(f"  QuantLib NPV:       {ql_npv:>12.2f}")
    print(f"  Quantra NPV:        {q_npv:>12.2f}")
    print(f"  NPV Difference:     {npv_diff:>12.4f}")
    print(f"  QuantLib Fair Rate: {ql_fair_rate * 100:>11.4f}%")
    print(f"  Quantra Fair Rate:  {q_fair_rate * 100:>11.4f}%")
    print(f"  Rate Difference:    {rate_diff * 10000:>11.4f} bps")
    print(f"  Leg NPVs (fixed, floating): QuantLib {ql_leg_npvs[0]:.2f} / {ql_leg_npvs[1]:.2f}"
          f"  Quantra {q_leg_npvs[0]:.2f} / {q_leg_npvs[1]:.2f}")
    passed = (npv_diff < TOLERANCE and rate_diff < 1e-6 and leg_diff < TOLERANCE
              and q_leg_npvs[0] < 0 < q_leg_npvs[1]
              and abs(q_leg_npvs[0] + q_leg_npvs[1] - q_npv) < 1e-6)
    print(f"  Result:             {'✓ PASS' if passed else '✗ FAIL'}")
    return passed, ql_npv, q_npv


def test_ois_swap(client: Client, curve_handle: ql.YieldTermStructureHandle):
    """
    Compare OIS pricing: QuantLib OvernightIndexedSwap vs Quantra

    Swap: 5Y payer, 4.5% fixed (ACT/360, annual) vs USD SOFR compounded
    annually, 10M notional, USD curve flat 4.5%. Both legs are SwapLeg
    entries in USD (the index's currency) on the same curve; the fixed leg
    states the overnight leg's payment calendar and lag (as the converter
    writes them), so both legs pay on the same dates as QuantLib's
    OvernightIndexedSwap.
    """
    print("\n" + "=" * 60)
    print("OIS SWAP")
    print("=" * 60)

    notional = 10_000_000.0
    fixed_rate = 0.045
    usd_rate = 0.045
    start = ql.Date(17, ql.January, 2025)
    end = ql.Date(17, ql.January, 2030)

    # --- QuantLib ---
    usd_curve = build_quantlib_curve(usd_rate)
    schedule_ql = ql.Schedule(start, end, ql.Period(ql.Annual), ql.TARGET(),
                              ql.ModifiedFollowing, ql.ModifiedFollowing,
                              ql.DateGeneration.Forward, False)
    sofr = ql.OvernightIndex("SOFR", 0, ql.USDCurrency(),
                             ql.UnitedStates(ql.UnitedStates.GovernmentBond), ql.Actual360(),
                             usd_curve)
    ql_swap = ql.OvernightIndexedSwap(ql.Swap.Payer, notional, schedule_ql, fixed_rate,
                                      ql.Actual360(), sofr, 0.0, 0, ql.ModifiedFollowing,
                                      ql.TARGET())
    ql_swap.setPricingEngine(ql.DiscountingSwapEngine(usd_curve))
    ql_npv = ql_swap.NPV()
    ql_fair_rate = ql_swap.fairRate()
    ql_leg_npvs = [ql_swap.legNPV(0), ql_swap.legNPV(1)]

    # --- Quantra ---
    def schedule():
        sch = ScheduleT()
        sch.effectiveDate = "2025-01-17"
        sch.terminationDate = "2030-01-17"
        sch.calendar = Calendar.TARGET
        sch.frequency = Frequency.Annual
        sch.convention = BusinessDayConvention.ModifiedFollowing
        sch.terminationDateConvention = BusinessDayConvention.ModifiedFollowing
        sch.dateGenerationRule = DateGenerationRule.Forward
        sch.endOfMonth = False
        return sch

    # On price-ois-swap both legs state their payment calendar and lag (the
    # leg-level fields); the coupon tables carry only the rate / the
    # overnight index and its compounding parameters.
    fixed_coupons = FixedCouponsT()
    fixed_coupons.rate = fixed_rate
    fixed_leg = SwapLegT()
    fixed_leg.payReceive = PayReceive.Pay
    fixed_leg.currency = "USD"
    fixed_leg.notional = notional
    fixed_leg.schedule = schedule()
    fixed_leg.dayCounter = DayCounter.Actual360
    fixed_leg.paymentConvention = BusinessDayConvention.ModifiedFollowing
    fixed_leg.paymentCalendar = Calendar.TARGET
    fixed_leg.paymentLag = 0
    fixed_leg.couponsType = SwapCoupons.FixedCoupons
    fixed_leg.coupons = fixed_coupons
    fixed_leg.discountingCurve = "usd_discount"

    sofr_ref = IndexRefT()
    sofr_ref.id = "USD_SOFR"
    overnight_coupons = OvernightCouponsT()
    overnight_coupons.index = sofr_ref
    overnight_coupons.spread = 0.0
    overnight_coupons.averagingMethod = RateAveragingType.Compound
    overnight_coupons.lookbackDays = 0
    overnight_coupons.lockoutDays = 0
    overnight_coupons.applyObservationShift = False
    overnight_leg = SwapLegT()
    overnight_leg.payReceive = PayReceive.Receive
    overnight_leg.currency = "USD"
    overnight_leg.notional = notional
    overnight_leg.schedule = schedule()
    overnight_leg.dayCounter = DayCounter.Actual360
    overnight_leg.paymentConvention = BusinessDayConvention.ModifiedFollowing
    overnight_leg.paymentCalendar = Calendar.TARGET
    overnight_leg.paymentLag = 0
    overnight_leg.couponsType = SwapCoupons.OvernightCoupons
    overnight_leg.coupons = overnight_coupons
    overnight_leg.discountingCurve = "usd_discount"
    overnight_leg.forwardingCurve = "usd_discount"

    swap = OisSwapT()
    swap.fixedLeg = fixed_leg
    swap.overnightLeg = overnight_leg
    price_swap = PriceOisSwapT()
    price_swap.oisSwap = swap

    request = PriceOisSwapRequestT()
    # The shared curve builder's swap helpers reference EUR_6M, so its
    # definition travels with the request next to the SOFR index.
    request.pricing = build_quantra_pricing([build_quantra_curve("usd_discount", usd_rate)],
                                            indices=[build_index_def_eur6m(),
                                                     build_index_def_sofr()])
    request.swaps = [price_swap]

    response = client.price_ois_swaps(request)
    result = response.Swaps(0)
    q_npv = result.Npv()
    q_fair_rate = result.FairRate()
    q_leg_npvs = [result.Legs(0).Npv(), result.Legs(1).Npv()]

    # --- Compare ---
    npv_diff = abs(ql_npv - q_npv)
    rate_diff = abs(ql_fair_rate - q_fair_rate)
    leg_diff = max(abs(a - b) for a, b in zip(ql_leg_npvs, q_leg_npvs))

    print(f"  QuantLib NPV:       {ql_npv:>12.2f}")
    print(f"  Quantra NPV:        {q_npv:>12.2f}")
    print(f"  NPV Difference:     {npv_diff:>12.4f}")
    print(f"  QuantLib Fair Rate: {ql_fair_rate * 100:>11.4f}%")
    print(f"  Quantra Fair Rate:  {q_fair_rate * 100:>11.4f}%")
    print(f"  Leg NPVs (fixed, overnight): QuantLib {ql_leg_npvs[0]:.2f} / {ql_leg_npvs[1]:.2f}"
          f"  Quantra {q_leg_npvs[0]:.2f} / {q_leg_npvs[1]:.2f}")
    passed = (npv_diff < TOLERANCE and rate_diff < 1e-6 and leg_diff < TOLERANCE
              and abs(q_leg_npvs[0] + q_leg_npvs[1] - q_npv) < 1e-6)
    print(f"  Result:             {'✓ PASS' if passed else '✗ FAIL'}")
    return passed, ql_npv, q_npv


def test_fra(client: Client, curve_handle: ql.YieldTermStructureHandle):
    """
    Compare FRA pricing: QuantLib vs Quantra
    
    FRA: 3x6, strike 3.2%, 1M notional
    """
    print("\n" + "=" * 60)
    print("FRA")
    print("=" * 60)
    
    notional = 1_000_000.0
    strike = 0.032
    val_date = EVAL_DATE + ql.Period(3, ql.Months)
    mat_date = EVAL_DATE + ql.Period(6, ql.Months)
    
    # --- QuantLib ---
    index = ql.Euribor3M(curve_handle)
    
    ql_fra = ql.ForwardRateAgreement(
        index, val_date, mat_date, ql.Position.Long, strike, notional, curve_handle
    )
    ql_npv = ql_fra.NPV()
    ql_fwd = ql_fra.forwardRate().rate()
    
    # --- Quantra ---
    fra = FRAT()
    fra.fraType = FRAType.Long
    fra.notional = notional
    fra.startDate = "2025-04-15"
    fra.maturityDate = "2025-07-15"
    fra.strike = strike
    fra.index = build_quantra_index_3m()
    fra.dayCounter = DayCounter.Actual360
    fra.calendar = Calendar.TARGET
    fra.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    
    price_fra = PriceFRAT()
    price_fra.fra = fra
    price_fra.discountingCurve = "discount"
    price_fra.forwardingCurve = "discount"
    
    request = PriceFRARequestT()
    request.pricing = build_quantra_pricing([build_quantra_curve("discount")], indices=[build_index_def_eur3m(), build_index_def_eur6m()])
    request.fras = [price_fra]
    
    response = client.price_fras(request)
    result = response.Fras(0)
    q_npv = result.Npv()
    q_fwd = result.ForwardRate()
    
    # --- Compare ---
    npv_diff = abs(ql_npv - q_npv)
    fwd_diff = abs(ql_fwd - q_fwd)
    
    print(f"  QuantLib NPV:       {ql_npv:>12.2f}")
    print(f"  Quantra NPV:        {q_npv:>12.2f}")
    print(f"  NPV Difference:     {npv_diff:>12.4f}")
    print(f"  QuantLib Fwd Rate:  {ql_fwd * 100:>11.4f}%")
    print(f"  Quantra Fwd Rate:   {q_fwd * 100:>11.4f}%")
    print(f"  Fwd Difference:     {fwd_diff * 10000:>11.4f} bps")
    
    passed = npv_diff < TOLERANCE and fwd_diff < 1e-6
    print(f"  Result:             {'✓ PASS' if passed else '✗ FAIL'}")
    return passed, ql_npv, q_npv


def test_cap(client: Client, curve_handle: ql.YieldTermStructureHandle):
    """
    Compare Cap pricing: QuantLib vs Quantra
    
    Cap: 5Y quarterly, 4% strike, 20% vol, 1M notional
    """
    print("\n" + "=" * 60)
    print("CAP")
    print("=" * 60)
    
    notional = 1_000_000.0
    strike = 0.04
    vol = 0.20
    start = EVAL_DATE + 2
    end = start + ql.Period(5, ql.Years)
    
    # --- QuantLib ---
    schedule = ql.Schedule(
        start, end, ql.Period(ql.Quarterly), ql.TARGET(),
        ql.ModifiedFollowing, ql.ModifiedFollowing, ql.DateGeneration.Forward, False
    )
    index = ql.Euribor3M(curve_handle)
    
    leg = ql.IborLeg([notional], schedule, index, ql.Actual360())
    ql_cap = ql.Cap(leg, [strike])
    
    vol_handle = ql.OptionletVolatilityStructureHandle(
        ql.ConstantOptionletVolatility(
            EVAL_DATE, ql.TARGET(), ql.ModifiedFollowing, vol, ql.Actual365Fixed()
        )
    )
    engine = ql.BlackCapFloorEngine(curve_handle, vol_handle)
    ql_cap.setPricingEngine(engine)
    ql_npv = ql_cap.NPV()
    
    # --- Quantra ---
    schedule_q = ScheduleT()
    schedule_q.effectiveDate = "2025-01-17"
    schedule_q.terminationDate = "2030-01-17"
    schedule_q.calendar = Calendar.TARGET
    schedule_q.frequency = Frequency.Quarterly
    schedule_q.convention = BusinessDayConvention.ModifiedFollowing
    schedule_q.terminationDateConvention = BusinessDayConvention.ModifiedFollowing
    schedule_q.dateGenerationRule = DateGenerationRule.Forward
    schedule_q.endOfMonth = False
    
    cap = CapFloorT()
    cap.capFloorType = CapFloorType.Cap
    cap.notional = notional
    cap.strike = strike
    cap.schedule = schedule_q
    cap.index = build_quantra_index_3m()
    cap.dayCounter = DayCounter.Actual360
    cap.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    
    price_cap = PriceCapFloorT()
    price_cap.capFloor = cap
    price_cap.discountingCurve = "discount"
    price_cap.forwardingCurve = "discount"
    price_cap.volatility = "cap_vol"
    price_cap.model = "black_model"  # NEW: model reference
    
    request = PriceCapFloorRequestT()
    request.pricing = build_quantra_pricing(
        curves=[build_quantra_curve("discount")],
        vol_surfaces=[build_quantra_optionlet_vol("cap_vol", vol)],
        models=[build_quantra_cap_floor_model("black_model", IrModelType.Black)],
        indices=[build_index_def_eur3m(), build_index_def_eur6m()]
    )
    request.capFloors = [price_cap]
    
    response = client.price_cap_floors(request)
    q_npv = response.CapFloors(0).Npv()
    
    # --- Compare ---
    diff = abs(ql_npv - q_npv)
    print(f"  QuantLib NPV:  {ql_npv:>12.2f}")
    print(f"  Quantra NPV:   {q_npv:>12.2f}")
    print(f"  Difference:    {diff:>12.4f}")
    
    passed = diff < TOLERANCE
    print(f"  Result:        {'✓ PASS' if passed else '✗ FAIL'}")
    return passed, ql_npv, q_npv


def test_swaption(client: Client, curve_handle: ql.YieldTermStructureHandle):
    """
    Compare Swaption pricing: QuantLib vs Quantra

    Swaption: 1Y into 5Y payer, 3.5% strike, 20% vol, 1M notional. The
    underlying is a VanillaSwap in the SwapLeg shape: both legs name the
    same discounting curve, the floating leg its forwarding curve; the
    PriceSwaption wrapper carries only the volatility and model ids.
    """
    print("\n" + "=" * 60)
    print("SWAPTION")
    print("=" * 60)
    
    notional = 1_000_000.0
    strike = 0.035
    vol = 0.20
    ex_date = EVAL_DATE + ql.Period(1, ql.Years)
    swap_start = ex_date + 2
    swap_end = swap_start + ql.Period(5, ql.Years)
    
    # --- QuantLib ---
    fixed_schedule = ql.Schedule(
        swap_start, swap_end, ql.Period(ql.Annual), ql.TARGET(),
        ql.ModifiedFollowing, ql.ModifiedFollowing, ql.DateGeneration.Forward, False
    )
    float_schedule = ql.Schedule(
        swap_start, swap_end, ql.Period(ql.Semiannual), ql.TARGET(),
        ql.ModifiedFollowing, ql.ModifiedFollowing, ql.DateGeneration.Forward, False
    )
    
    index = ql.Euribor6M(curve_handle)
    
    underlying = ql.VanillaSwap(
        ql.VanillaSwap.Payer, notional,
        fixed_schedule, strike, ql.Thirty360(ql.Thirty360.BondBasis),
        float_schedule, index, 0.0, ql.Actual360()
    )
    
    exercise = ql.EuropeanExercise(ex_date)
    ql_swaption = ql.Swaption(underlying, exercise)
    
    vol_handle = ql.SwaptionVolatilityStructureHandle(
        ql.ConstantSwaptionVolatility(
            EVAL_DATE, ql.TARGET(), ql.ModifiedFollowing, vol, ql.Actual365Fixed()
        )
    )
    engine = ql.BlackSwaptionEngine(curve_handle, vol_handle)
    ql_swaption.setPricingEngine(engine)
    ql_npv = ql_swaption.NPV()
    
    # --- Quantra ---
    fixed_schedule_q = ScheduleT()
    fixed_schedule_q.effectiveDate = "2026-01-17"
    fixed_schedule_q.terminationDate = "2031-01-17"
    fixed_schedule_q.calendar = Calendar.TARGET
    fixed_schedule_q.frequency = Frequency.Annual
    fixed_schedule_q.convention = BusinessDayConvention.ModifiedFollowing
    fixed_schedule_q.terminationDateConvention = BusinessDayConvention.ModifiedFollowing
    fixed_schedule_q.dateGenerationRule = DateGenerationRule.Forward
    fixed_schedule_q.endOfMonth = False
    
    fixed_coupons = FixedCouponsT()
    fixed_coupons.rate = strike
    
    float_schedule_q = ScheduleT()
    float_schedule_q.effectiveDate = "2026-01-17"
    float_schedule_q.terminationDate = "2031-01-17"
    float_schedule_q.calendar = Calendar.TARGET
    float_schedule_q.frequency = Frequency.Semiannual
    float_schedule_q.convention = BusinessDayConvention.ModifiedFollowing
    float_schedule_q.terminationDateConvention = BusinessDayConvention.ModifiedFollowing
    float_schedule_q.dateGenerationRule = DateGenerationRule.Forward
    float_schedule_q.endOfMonth = False
    
    ibor_coupons = IborCouponsT()
    ibor_coupons.index = build_quantra_index_6m()
    ibor_coupons.spread = 0.0

    # The underlying is built from the same flat SwapLegT as the swap
    # endpoints: notional / schedule / conventions on the leg.
    fixed_leg = SwapLegT()
    fixed_leg.payReceive = PayReceive.Pay          # payer swaption
    fixed_leg.currency = "EUR"
    fixed_leg.notional = notional
    fixed_leg.schedule = fixed_schedule_q
    fixed_leg.dayCounter = DayCounter.Thirty360
    fixed_leg.paymentConvention = BusinessDayConvention.ModifiedFollowing
    fixed_leg.couponsType = SwapCoupons.FixedCoupons
    fixed_leg.coupons = fixed_coupons
    fixed_leg.discountingCurve = "discount"

    floating_leg = SwapLegT()
    floating_leg.payReceive = PayReceive.Receive
    floating_leg.currency = "EUR"
    floating_leg.notional = notional
    floating_leg.schedule = float_schedule_q
    floating_leg.dayCounter = DayCounter.Actual360
    floating_leg.paymentConvention = BusinessDayConvention.ModifiedFollowing
    floating_leg.couponsType = SwapCoupons.IborCoupons
    floating_leg.coupons = ibor_coupons
    floating_leg.discountingCurve = "discount"
    floating_leg.forwardingCurve = "discount"

    underlying = VanillaSwapT()
    underlying.fixedLeg = fixed_leg
    underlying.floatingLeg = floating_leg
    
    swaption = SwaptionT()
    swaption.underlyingType = 1
    swaption.underlying = underlying
    swaption.exerciseDate = "2026-01-15"
    swaption.exerciseType = ExerciseType.European
    swaption.settlementType = SettlementType.Physical
    
    price_swaption = PriceSwaptionT()
    price_swaption.swaption = swaption
    price_swaption.volatility = "swaption_vol"
    price_swaption.model = "black_model"  # NEW: model reference
    
    request = PriceSwaptionRequestT()
    request.pricing = build_quantra_pricing(
        curves=[build_quantra_curve("discount")],
        vol_surfaces=[build_quantra_swaption_vol("swaption_vol", vol)],
        models=[build_quantra_swaption_model("black_model", IrModelType.Black)],
        indices=[build_index_def_eur6m()]
    )
    request.swaptions = [price_swaption]
    
    response = client.price_swaptions(request)
    q_npv = response.Swaptions(0).Npv()
    
    # --- Compare ---
    diff = abs(ql_npv - q_npv)
    print(f"  QuantLib NPV:  {ql_npv:>12.2f}")
    print(f"  Quantra NPV:   {q_npv:>12.2f}")
    print(f"  Difference:    {diff:>12.4f}")
    
    passed = diff < TOLERANCE
    print(f"  Result:        {'✓ PASS' if passed else '✗ FAIL'}")
    return passed, ql_npv, q_npv


def test_cds(client: Client, curve_handle: ql.YieldTermStructureHandle):
    """
    Compare CDS pricing: QuantLib vs Quantra
    
    CDS: 5Y protection buyer, 100bps spread, 40% recovery, 2% hazard, 10M notional
    """
    print("\n" + "=" * 60)
    print("CDS")
    print("=" * 60)
    
    notional = 10_000_000.0
    spread = 0.01  # 100 bps
    recovery = 0.40
    hazard = 0.02
    start = EVAL_DATE
    end = start + ql.Period(5, ql.Years)
    
    # --- QuantLib ---
    schedule = ql.Schedule(
        start, end, ql.Period(ql.Quarterly), ql.TARGET(),
        ql.Following, ql.Unadjusted, ql.DateGeneration.TwentiethIMM, False
    )
    
    ql_cds = ql.CreditDefaultSwap(
        ql.Protection.Buyer, notional, spread, schedule, ql.Following, ql.Actual360()
    )
    
    hazard_quote = ql.QuoteHandle(ql.SimpleQuote(hazard))
    default_curve = ql.FlatHazardRate(EVAL_DATE, hazard_quote, ql.Actual365Fixed())
    default_handle = ql.DefaultProbabilityTermStructureHandle(default_curve)
    
    engine = ql.MidPointCdsEngine(default_handle, recovery, curve_handle)
    ql_cds.setPricingEngine(engine)
    ql_npv = ql_cds.NPV()
    ql_fair = ql_cds.fairSpread()
    
    # --- Quantra ---
    schedule_q = ScheduleT()
    schedule_q.effectiveDate = "2025-01-15"
    schedule_q.terminationDate = "2030-01-15"
    schedule_q.calendar = Calendar.TARGET
    schedule_q.frequency = Frequency.Quarterly
    schedule_q.convention = BusinessDayConvention.Following
    schedule_q.terminationDateConvention = BusinessDayConvention.Unadjusted
    schedule_q.dateGenerationRule = DateGenerationRule.TwentiethIMM
    schedule_q.endOfMonth = False
    
    cds = CDST()
    cds.side = ProtectionSide.Buyer
    cds.notional = notional
    cds.runningCoupon = spread
    cds.schedule = schedule_q
    cds.dayCounter = DayCounter.Actual360
    cds.businessDayConvention = BusinessDayConvention.Following
    cds.settlesAccrual = True
    cds.paysAtDefaultTime = True
    cds.rebatesAccrual = True
    cds.lastPeriodDayCounter = DayCounter.Actual360
    cds.cashSettlementDays = 3

    credit_curve = CreditCurveSpecT()
    credit_curve.id = "credit"
    credit_curve.referenceDate = EVAL_DATE_STR
    credit_curve.calendar = Calendar.TARGET
    credit_curve.dayCounter = DayCounter.Actual365Fixed
    credit_curve.recoveryRate = recovery
    credit_curve.curveInterpolator = Interpolator.LogLinear
    credit_curve.flatHazardRate = hazard
    helper_conv = CdsHelperConventionsT()
    helper_conv.settlementDays = 0
    helper_conv.frequency = Frequency.Quarterly
    helper_conv.businessDayConvention = BusinessDayConvention.Following
    helper_conv.dateGenerationRule = DateGenerationRule.TwentiethIMM
    helper_conv.lastPeriodDayCounter = DayCounter.Actual365Fixed
    helper_conv.settlesAccrual = True
    helper_conv.paysAtDefaultTime = True
    helper_conv.rebatesAccrual = True
    helper_conv.helperModel = CdsHelperModel.MidPoint
    credit_curve.helperConventions = helper_conv
    credit_curve.quotes = []

    cds_model = CdsModelSpecT()
    cds_model.engineType = CdsEngineType.MidPoint
    model_spec = ModelSpecT()
    model_spec.id = "cds_model"
    model_spec.payloadType = ModelPayload.CdsModelSpec
    model_spec.payload = cds_model
    
    price_cds = PriceCDST()
    price_cds.cds = cds
    price_cds.discountingCurve = "discount"
    price_cds.creditCurveId = "credit"
    price_cds.model = "cds_model"
    
    request = PriceCDSRequestT()
    request.pricing = build_quantra_pricing(
        [build_quantra_curve("discount")],
        indices=[build_index_def_eur6m()],
        credit_curves=[credit_curve],
        models=[model_spec]
    )
    request.cdsList = [price_cds]
    
    response = client.price_cds(request)
    result = response.CdsList(0)
    q_npv = result.Npv()
    q_fair = result.FairSpread()
    
    # --- Compare ---
    npv_diff = abs(ql_npv - q_npv)
    fair_diff = abs(ql_fair - q_fair)
    
    print(f"  QuantLib NPV:        {ql_npv:>12.2f}")
    print(f"  Quantra NPV:         {q_npv:>12.2f}")
    print(f"  NPV Difference:      {npv_diff:>12.4f}")
    print(f"  QuantLib Fair:       {ql_fair * 10000:>11.2f} bps")
    print(f"  Quantra Fair:        {q_fair * 10000:>11.2f} bps")
    print(f"  Fair Difference:     {fair_diff * 10000:>11.4f} bps")
    
    passed = npv_diff < TOLERANCE and fair_diff < 1e-6
    print(f"  Result:              {'✓ PASS' if passed else '✗ FAIL'}")
    return passed, ql_npv, q_npv


# =============================================================================
# Test: Bootstrap Curves
# =============================================================================

def test_bootstrap_curves(client, curve_handle):
    """Test curve bootstrapping endpoint - compare discount factors."""
    print("\n--- Test: Bootstrap Curves ---")
    
    # Build Pricing + query request (v2 bootstrap schema)
    from quantra.BootstrapCurvesRequest import BootstrapCurvesRequestT
    from quantra.CurveQuerySpec import CurveQuerySpecT
    from quantra.DateGridSpec import DateGridSpecT
    from quantra.TenorGrid import TenorGridT
    from quantra.Period import PeriodT
    from quantra.DateGrid import DateGrid
    from quantra.CurveMeasure import CurveMeasure
    
    # Build structured tenors (new format: {n, unit})
    tenors = []
    for n, unit in [(3, TimeUnit.Months), (6, TimeUnit.Months), (1, TimeUnit.Years), 
                    (2, TimeUnit.Years), (5, TimeUnit.Years)]:
        tenor = PeriodT()
        tenor.n = n
        tenor.unit = unit
        tenors.append(tenor)
    
    # Build tenor grid
    tenor_grid = TenorGridT()
    tenor_grid.tenors = tenors
    
    # Build grid spec
    grid_spec = DateGridSpecT()
    grid_spec.gridType = DateGrid.TenorGrid
    grid_spec.grid = tenor_grid
    
    # Build query
    query = CurveQuerySpecT()
    query.curveId = "test_curve"
    query.measures = [CurveMeasure.DF]
    query.grid = grid_spec
    
    # Build request
    request = BootstrapCurvesRequestT()
    request.pricing = build_quantra_pricing([build_quantra_curve("test_curve")], indices=[build_index_def_eur6m()])
    request.queries = [query]
    
    response = client.bootstrap_curves(request)
    
    # Compare discount factors
    result = response.Results(0)
    print(f"  Curve ID: {result.Id()}")
    print(f"  Reference Date: {result.ReferenceDate()}")
    print(f"  Grid Points: {result.GridDatesLength()}")
    
    if result.Error():
        print(f"  ERROR: {result.Error().ErrorMessage()}")
        return False, 0, 0
    
    # Get DF series
    df_series = None
    for i in range(result.SeriesLength()):
        series = result.Series(i)
        if series.Measure() == CurveMeasure.DF:
            df_series = series
            break
    
    if not df_series:
        print("  ERROR: No DF series returned")
        return False, 0, 0
    
    max_diff = 0.0
    ql_first = None
    q_first = None
    
    for i in range(result.GridDatesLength()):
        date_str = result.GridDates(i).decode() if isinstance(result.GridDates(i), bytes) else result.GridDates(i)
        q_df = df_series.Values(i)
        
        # Parse date (format: YYYY-MM-DD from iso_date)
        parts = date_str.split('-')
        ql_date = ql.Date(int(parts[2]), int(parts[1]), int(parts[0]))
        ql_df = curve_handle.discount(ql_date)
        
        if i == 0:
            ql_first = ql_df
            q_first = q_df
        
        diff = abs(ql_df - q_df)
        max_diff = max(max_diff, diff)
        print(f"    {date_str}: QL={ql_df:.8f}, Q={q_df:.8f}, Diff={diff:.2e}")
    
    passed = max_diff < 1e-6
    print(f"  Max Difference: {max_diff:.2e}")
    print(f"  Result:         {'✓ PASS' if passed else '✗ FAIL'}")
    
    return passed, ql_first or 0, q_first or 0


def test_fx_forward(client: Client, curve_handle: ql.YieldTermStructureHandle):
    """
    Compare FX forward pricing: QuantLib discount-factor algebra vs Quantra

    EUR/USD 6M, buy 10m EUR at 1.0900, spot 1.0850 T+2, EUR curve flat 3%,
    USD curve flat 4.5%, NPV in USD. The reference is the identity the
    engine documents (docs/http-api.md, FX forward):
        X    = S * DF_usd(spot) / DF_eur(spot)
        fair = X * DF_eur(T) / DF_usd(T)
        npv  = N * DF_eur(T) * X - N * K * DF_usd(T)
    """
    print("\n" + "=" * 60)
    print("FX Forward")
    print("=" * 60)

    notional = 10_000_000.0
    strike = 1.0900
    spot = 1.0850
    usd_rate = 0.045

    # --- QuantLib ---
    eur_curve = curve_handle                      # flat 3% (the shared fixture)
    usd_curve = build_quantlib_curve(usd_rate)    # flat 4.5%
    cal = ql.JointCalendar(ql.TARGET(), ql.UnitedStates(ql.UnitedStates.Settlement), ql.JoinHolidays)
    spot_date = cal.advance(cal.adjust(EVAL_DATE), ql.Period(2, ql.Days))
    maturity = cal.adjust(ql.Date(17, ql.July, 2025), ql.ModifiedFollowing)
    X = spot * usd_curve.discount(spot_date) / eur_curve.discount(spot_date)
    ql_fair = X * eur_curve.discount(maturity) / usd_curve.discount(maturity)
    ql_npv = notional * eur_curve.discount(maturity) * X - notional * strike * usd_curve.discount(maturity)

    # --- Quantra ---
    fx_spot = FxSpotSpecT()
    fx_spot.id = "EURUSD_SPOT"
    fx_spot.baseCurrency = "EUR"
    fx_spot.quoteCurrency = "USD"
    fx_spot.rate = spot
    fx_spot.spotDays = 2
    fx_spot.settlementCalendars = [Calendar.TARGET, Calendar.UnitedStatesSettlement]
    fx_spot.businessDayConvention = BusinessDayConvention.ModifiedFollowing

    fwd = FxForwardT()
    fwd.baseCurrency = "EUR"
    fwd.quoteCurrency = "USD"
    fwd.baseNotional = notional
    fwd.forwardRate = strike
    fwd.maturityDate = "2025-07-17"
    fwd.direction = FxDirection.BuyBase

    price_fwd = PriceFxForwardT()
    price_fwd.fxForward = fwd
    price_fwd.fxSpot = "EURUSD_SPOT"
    price_fwd.baseDiscountingCurve = "discount"
    price_fwd.quoteDiscountingCurve = "usd_discount"
    price_fwd.reportingCurrency = "USD"

    request = PriceFxForwardRequestT()
    request.pricing = build_quantra_pricing(
        [build_quantra_curve("discount"), build_quantra_curve("usd_discount", usd_rate)],
        indices=[build_index_def_eur3m(), build_index_def_eur6m()],
        fx_spots=[fx_spot])
    request.fxForwards = [price_fwd]

    response = client.price_fx_forwards(request)
    result = response.FxForwards(0)
    q_npv = result.Npv()
    q_fair = result.FairForwardRate()

    # --- Compare ---
    npv_diff = abs(ql_npv - q_npv)
    fair_diff = abs(ql_fair - q_fair)

    print(f"  Spot date / maturity: {result.SpotDate().decode()} / {result.MaturityDate().decode()}")
    print(f"  QuantLib NPV (USD): {ql_npv:>12.2f}")
    print(f"  Quantra NPV (USD):  {q_npv:>12.2f}")
    print(f"  NPV Difference:     {npv_diff:>12.6f}")
    print(f"  QuantLib fair fwd:  {ql_fair:>12.6f}")
    print(f"  Quantra fair fwd:   {q_fair:>12.6f}")
    print(f"  Forward points:     {result.ForwardPoints():>12.6f}")

    passed = (npv_diff < 1e-6 and fair_diff < 1e-12
              and result.SpotDate().decode() == spot_date.ISO()
              and result.MaturityDate().decode() == maturity.ISO())
    print(f"  Result:             {'✓ PASS' if passed else '✗ FAIL'}")

    return passed, ql_npv, q_npv


def test_cross_currency_swap(client: Client, curve_handle: ql.YieldTermStructureHandle):
    """
    Compare cross-currency swap pricing: QuantLib cash-flow algebra vs Quantra

    Two SwapLeg entries: pay EUR 2.55% annual (30/360) on 10m EUR
    (FixedCoupons), receive USD SOFR compounded quarterly on 10.85m USD
    (OvernightCoupons, the OIS leg conventions), both principal exchanges,
    5Y from 2025-01-17, EUR curve flat 3%, USD curve flat 4.5%, spot 1.0850
    T+2, NPV in USD. The reference is the identity the engine documents
    (docs/http-api.md, Cross-currency swap): each leg (coupons + exchanges)
    NPV'd on its own curve with ql.CashFlows.npv, the EUR leg converted at
    X = S * DF_usd(spot) / DF_eur(spot), and the fair rate from the fixed
    leg's annuity.
    """
    print("\n" + "=" * 60)
    print("Cross-Currency Swap")
    print("=" * 60)

    eur_notional = 10_000_000.0
    spot = 1.0850
    usd_notional = eur_notional * spot
    fixed_rate = 0.0255
    usd_rate = 0.045
    start = ql.Date(17, ql.January, 2025)
    end = ql.Date(17, ql.January, 2030)

    # --- QuantLib ---
    eur_curve = curve_handle                      # flat 3% (the shared fixture)
    usd_curve = build_quantlib_curve(usd_rate)    # flat 4.5%
    fixed_schedule = ql.Schedule(start, end, ql.Period(ql.Annual), ql.TARGET(),
                                 ql.ModifiedFollowing, ql.ModifiedFollowing,
                                 ql.DateGeneration.Forward, False)
    float_schedule = ql.Schedule(start, end, ql.Period(ql.Quarterly), ql.TARGET(),
                                 ql.ModifiedFollowing, ql.ModifiedFollowing,
                                 ql.DateGeneration.Forward, False)
    sofr = ql.OvernightIndex("SOFR", 0, ql.USDCurrency(),
                             ql.UnitedStates(ql.UnitedStates.GovernmentBond), ql.Actual360(),
                             usd_curve)
    eur_coupons = ql.FixedRateLeg(fixed_schedule, ql.Thirty360(ql.Thirty360.BondBasis),
                                  [eur_notional], [fixed_rate], ql.ModifiedFollowing)
    usd_coupons = ql.OvernightLeg([usd_notional], float_schedule, sofr, sofr.dayCounter(),
                                  ql.ModifiedFollowing, [1.0], [0.0], False,
                                  ql.RateAveraging.Compound, ql.TARGET(), 0)
    eur_leg = list(eur_coupons) + [ql.SimpleCashFlow(-eur_notional, start),
                                   ql.SimpleCashFlow(eur_notional, eur_coupons[-1].date())]
    usd_leg = list(usd_coupons) + [ql.SimpleCashFlow(-usd_notional, start),
                                   ql.SimpleCashFlow(usd_notional, usd_coupons[-1].date())]
    cal = ql.JointCalendar(ql.TARGET(), ql.UnitedStates(ql.UnitedStates.Settlement), ql.JoinHolidays)
    spot_date = cal.advance(cal.adjust(EVAL_DATE), ql.Period(2, ql.Days))
    X = spot * usd_curve.discount(spot_date) / eur_curve.discount(spot_date)
    eur_pv = -ql.CashFlows.npv(eur_leg, eur_curve, False, EVAL_DATE, EVAL_DATE)   # paid
    usd_pv = ql.CashFlows.npv(usd_leg, usd_curve, False, EVAL_DATE, EVAL_DATE)    # received
    ql_npv = eur_pv * X + usd_pv
    eur_annuity = ql.CashFlows.bps(eur_coupons, eur_curve, False, EVAL_DATE, EVAL_DATE) / 1e-4
    ql_fair_rate = fixed_rate - (eur_pv + usd_pv / X) / (-1.0 * eur_annuity)

    # --- Quantra ---
    fx_spot = FxSpotSpecT()
    fx_spot.id = "EURUSD_SPOT"
    fx_spot.baseCurrency = "EUR"
    fx_spot.quoteCurrency = "USD"
    fx_spot.rate = spot
    fx_spot.spotDays = 2
    fx_spot.settlementCalendars = [Calendar.TARGET, Calendar.UnitedStatesSettlement]
    fx_spot.businessDayConvention = BusinessDayConvention.ModifiedFollowing

    def schedule(frequency):
        sch = ScheduleT()
        sch.effectiveDate = "2025-01-17"
        sch.terminationDate = "2030-01-17"
        sch.calendar = Calendar.TARGET
        sch.frequency = frequency
        sch.convention = BusinessDayConvention.ModifiedFollowing
        sch.terminationDateConvention = BusinessDayConvention.ModifiedFollowing
        sch.dateGenerationRule = DateGenerationRule.Forward
        sch.endOfMonth = False
        return sch

    fixed_coupons = FixedCouponsT()
    fixed_coupons.rate = fixed_rate
    leg1 = SwapLegT()
    leg1.payReceive = PayReceive.Pay
    leg1.currency = "EUR"
    leg1.notional = eur_notional
    leg1.schedule = schedule(Frequency.Annual)
    leg1.dayCounter = DayCounter.Thirty360
    leg1.paymentConvention = BusinessDayConvention.ModifiedFollowing
    leg1.couponsType = SwapCoupons.FixedCoupons
    leg1.coupons = fixed_coupons
    leg1.initialExchange = True
    leg1.finalExchange = True
    leg1.discountingCurve = "discount"

    sofr_ref = IndexRefT()
    sofr_ref.id = "USD_SOFR"
    overnight_coupons = OvernightCouponsT()
    overnight_coupons.index = sofr_ref
    overnight_coupons.spread = 0.0
    overnight_coupons.averagingMethod = RateAveragingType.Compound
    overnight_coupons.lookbackDays = 0
    overnight_coupons.lockoutDays = 0
    overnight_coupons.applyObservationShift = False
    leg2 = SwapLegT()
    leg2.payReceive = PayReceive.Receive
    leg2.currency = "USD"
    leg2.notional = usd_notional
    leg2.schedule = schedule(Frequency.Quarterly)
    leg2.dayCounter = DayCounter.Actual360
    leg2.paymentConvention = BusinessDayConvention.ModifiedFollowing
    leg2.paymentCalendar = Calendar.TARGET
    leg2.paymentLag = 0
    leg2.couponsType = SwapCoupons.OvernightCoupons
    leg2.coupons = overnight_coupons
    leg2.initialExchange = True
    leg2.finalExchange = True
    leg2.discountingCurve = "usd_discount"
    leg2.forwardingCurve = "usd_discount"

    swap = CrossCurrencySwapT()
    swap.leg1 = leg1
    swap.leg2 = leg2
    price_swap = PriceCrossCurrencySwapT()
    price_swap.crossCurrencySwap = swap
    price_swap.fxSpot = "EURUSD_SPOT"
    price_swap.reportingCurrency = "USD"

    request = PriceCrossCurrencySwapRequestT()
    request.pricing = build_quantra_pricing(
        [build_quantra_curve("discount"), build_quantra_curve("usd_discount", usd_rate)],
        indices=[build_index_def_eur3m(), build_index_def_eur6m(), build_index_def_sofr()],
        fx_spots=[fx_spot])
    request.swaps = [price_swap]
    request.includeFlows = True

    response = client.price_cross_currency_swaps(request)
    result = response.Swaps(0)
    q_npv = result.Npv()
    q_fair_rate = result.FairRate()
    leg1_result = result.Legs(0)
    flows = [leg1_result.Flows(i) for i in range(leg1_result.FlowsLength())]
    exchanges = [f for f in flows if f.Kind() == SwapFlowKind.NotionalExchange]

    # --- Compare ---
    npv_diff = abs(ql_npv - q_npv)
    rate_diff = abs(ql_fair_rate - q_fair_rate)

    print(f"  Spot date:          {result.SpotDate().decode()}  X = {result.FxAsOfRate():.8f}")
    print(f"  QuantLib NPV (USD): {ql_npv:>12.2f}")
    print(f"  Quantra NPV (USD):  {q_npv:>12.2f}")
    print(f"  NPV Difference:     {npv_diff:>12.6f}")
    print(f"  QuantLib fair rate: {ql_fair_rate * 100:>11.6f}%")
    print(f"  Quantra fair rate:  {q_fair_rate * 100:>11.6f}%")
    print(f"  EUR leg flows:      {len(flows)} ({len(exchanges)} notional exchanges)")

    passed = (npv_diff < 1e-6 and rate_diff < 1e-12
              and result.SpotDate().decode() == spot_date.ISO()
              and len(flows) == 7 and len(exchanges) == 2
              and exchanges[0].Amount() == -eur_notional)
    print(f"  Result:             {'✓ PASS' if passed else '✗ FAIL'}")

    return passed, ql_npv, q_npv


# =============================================================================
# Main
# =============================================================================

def main():
    print("=" * 60)
    print("Quantra vs QuantLib Python Comparison Tests")
    print("=" * 60)
    print(f"Evaluation Date: {EVAL_DATE}")
    print(f"Flat Rate:       {FLAT_RATE * 100:.1f}%")
    print(f"Tolerance:       {TOLERANCE}")
    
    # Setup
    curve_handle = setup_quantlib()
    client = Client("localhost:50051")
    
    # Run tests
    tests = [
        ("Fixed Rate Bond", lambda: test_fixed_rate_bond(client, curve_handle)),
        ("Vanilla Swap", lambda: test_vanilla_swap(client, curve_handle)),
        ("OIS Swap", lambda: test_ois_swap(client, curve_handle)),
        ("FRA", lambda: test_fra(client, curve_handle)),
        ("FX Forward", lambda: test_fx_forward(client, curve_handle)),
        ("Cross-Currency Swap", lambda: test_cross_currency_swap(client, curve_handle)),
        ("Cap", lambda: test_cap(client, curve_handle)),
        ("Swaption", lambda: test_swaption(client, curve_handle)),
        ("CDS", lambda: test_cds(client, curve_handle)),
        ("Bootstrap Curves", lambda: test_bootstrap_curves(client, curve_handle)),
    ]
    
    results = []
    for name, test_func in tests:
        try:
            passed, ql_val, q_val = test_func()
            results.append((name, passed, ql_val, q_val, None))
        except Exception as e:
            results.append((name, False, None, None, str(e)))
            print(f"  Error: {e}")
    
    # Summary
    print("\n" + "=" * 60)
    print("SUMMARY")
    print("=" * 60)
    print(f"{'Test':<20} {'QuantLib':>12} {'Quantra':>12} {'Diff':>10} {'Status':<8}")
    print("-" * 60)
    
    for name, passed, ql_val, q_val, error in results:
        if error:
            print(f"{name:<20} {'ERROR':>12} {'':<12} {'':<10} {'✗ FAIL':<8}")
        else:
            diff = abs(ql_val - q_val)
            status = '✓ PASS' if passed else '✗ FAIL'
            print(f"{name:<20} {ql_val:>12.2f} {q_val:>12.2f} {diff:>10.4f} {status:<8}")
    
    total = len(results)
    passed_count = sum(1 for _, p, _, _, _ in results if p)
    print("-" * 60)
    print(f"Total scenarios: {passed_count}/{total} passed")
    
    return passed_count == total


if __name__ == "__main__":
    success = main()
    exit(0 if success else 1)