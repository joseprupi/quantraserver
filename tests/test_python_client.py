"""
Quantra vs QuantLib Python Comparison Tests

Tests that Quantra FlatBuffers client produces identical results to raw QuantLib Python.
Uses identical bootstrapped curves for exact matching.

This mirrors the C++ comparison tests in tests/quantra_comparison_test.cpp

Usage:
    PYTHONPATH=/workspace/quantra-python:/workspace/flatbuffers/python python3 tests/test_quantra_vs_quantlib.py
"""

import sys
import os

# Get the project root directory (parent of tests/)
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)

# Add paths for imports
sys.path.insert(0, os.path.join(PROJECT_ROOT, 'flatbuffers/python'))
sys.path.insert(0, os.path.join(PROJECT_ROOT, 'quantra-python'))

import QuantLib as ql
from quantra_client.client import (
    Client,
    # Request types
    PriceFixedRateBondRequestT,
    PriceVanillaSwapRequestT,
    PriceFRARequestT,
    PriceCapFloorRequestT,
    PriceSwaptionRequestT,
    PriceCDSRequestT,
    # Building blocks
    PricingT,
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
    SwapFixedLegT,
    SwapFloatingLegT,
    PriceVanillaSwapT,
    FRAT,
    PriceFRAT,
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
    TimeUnit,
    DateGenerationRule,
    Interpolator,
    BootstrapTrait,
    Compounding,
    SwapType,
    FRAType,
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


# =============================================================================
# QuantLib Python Setup
# =============================================================================

def setup_quantlib():
    """Set up QuantLib evaluation date and build curve."""
    ql.Settings.instance().evaluationDate = EVAL_DATE
    return build_quantlib_curve()


def build_quantlib_curve():
    """Build the same bootstrapped curve as C++ test."""
    helpers = []
    
    # 3M deposit
    helpers.append(ql.DepositRateHelper(
        FLAT_RATE, ql.Period(3, ql.Months), 2, ql.TARGET(),
        ql.ModifiedFollowing, True, ql.Actual365Fixed()
    ))
    
    # 6M deposit
    helpers.append(ql.DepositRateHelper(
        FLAT_RATE, ql.Period(6, ql.Months), 2, ql.TARGET(),
        ql.ModifiedFollowing, True, ql.Actual365Fixed()
    ))
    
    # 1Y deposit
    helpers.append(ql.DepositRateHelper(
        FLAT_RATE, ql.Period(1, ql.Years), 2, ql.TARGET(),
        ql.ModifiedFollowing, True, ql.Actual365Fixed()
    ))
    
    # 5Y swap
    euribor6m = ql.Euribor6M()
    helpers.append(ql.SwapRateHelper(
        FLAT_RATE, ql.Period(5, ql.Years), ql.TARGET(), ql.Annual,
        ql.ModifiedFollowing, ql.Thirty360(ql.Thirty360.BondBasis), euribor6m
    ))
    
    # 10Y swap
    helpers.append(ql.SwapRateHelper(
        FLAT_RATE, ql.Period(10, ql.Years), ql.TARGET(), ql.Annual,
        ql.ModifiedFollowing, ql.Thirty360(ql.Thirty360.BondBasis), euribor6m
    ))
    
    curve = ql.PiecewiseLogLinearDiscount(EVAL_DATE, helpers, ql.Actual365Fixed())
    curve.enableExtrapolation()
    return ql.YieldTermStructureHandle(curve)


# =============================================================================
# Quantra Helpers
# =============================================================================

def build_quantra_curve(curve_id: str = "discount") -> TermStructureT:
    """Build the same curve for Quantra."""
    ts = TermStructureT()
    ts.id = curve_id
    ts.dayCounter = DayCounter.Actual365Fixed
    ts.interpolator = Interpolator.LogLinear
    ts.bootstrapTrait = BootstrapTrait.Discount
    ts.referenceDate = EVAL_DATE_STR
    ts.points = []
    
    # 3M deposit
    dep3m = DepositHelperT()
    dep3m.rate = FLAT_RATE
    dep3m.tenorNumber = 3
    dep3m.tenorTimeUnit = TimeUnit.Months
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
    dep6m.rate = FLAT_RATE
    dep6m.tenorNumber = 6
    dep6m.tenorTimeUnit = TimeUnit.Months
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
    dep1y.rate = FLAT_RATE
    dep1y.tenorNumber = 1
    dep1y.tenorTimeUnit = TimeUnit.Years
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
    sw5y.rate = FLAT_RATE
    sw5y.tenorNumber = 5
    sw5y.tenorTimeUnit = TimeUnit.Years
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
    sw10y.rate = FLAT_RATE
    sw10y.tenorNumber = 10
    sw10y.tenorTimeUnit = TimeUnit.Years
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
    idef.tenorNumber = 6
    idef.tenorTimeUnit = TimeUnit.Months
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
    idef.tenorNumber = 3
    idef.tenorTimeUnit = TimeUnit.Months
    idef.fixingDays = 2
    idef.calendar = Calendar.TARGET
    idef.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    idef.dayCounter = DayCounter.Actual360
    idef.endOfMonth = False
    idef.currency = "EUR"
    return idef

def build_quantra_pricing(curves: list, vol_surfaces: list = None, models: list = None,
                            indices: list = None, credit_curves: list = None) -> PricingT:
    """Build Pricing object for Quantra.
    
    Args:
        curves: List of TermStructureT
        vol_surfaces: List of VolSurfaceSpecT (for caps/floors/swaptions)
        models: List of ModelSpecT (for caps/floors/swaptions)
        indices: List of IndexDefT (index definitions referenced by helpers/instruments)
        credit_curves: List of CreditCurveSpecT (CDS credit curves)
    """
    p = PricingT()
    p.asOfDate = EVAL_DATE_STR
    p.settlementDate = EVAL_DATE_STR
    p.indices = indices or []
    p.curves = curves
    p.creditCurves = credit_curves or []
    p.volSurfaces = vol_surfaces or []
    p.models = models or []
    p.bondPricingDetails = True
    p.bondPricingFlows = False
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
    
    Swap: 5Y payer, 3.5% fixed vs 6M Euribor, 1M notional
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
    
    # --- Quantra ---
    fixed_schedule_q = ScheduleT()
    fixed_schedule_q.effectiveDate = "2025-01-17"
    fixed_schedule_q.terminationDate = "2030-01-17"
    fixed_schedule_q.calendar = Calendar.TARGET
    fixed_schedule_q.frequency = Frequency.Annual
    fixed_schedule_q.convention = BusinessDayConvention.ModifiedFollowing
    fixed_schedule_q.terminationDateConvention = BusinessDayConvention.ModifiedFollowing
    fixed_schedule_q.dateGenerationRule = DateGenerationRule.Forward
    
    fixed_leg = SwapFixedLegT()
    fixed_leg.notional = notional
    fixed_leg.schedule = fixed_schedule_q
    fixed_leg.rate = fixed_rate
    fixed_leg.dayCounter = DayCounter.Thirty360
    fixed_leg.paymentConvention = BusinessDayConvention.ModifiedFollowing
    
    float_schedule_q = ScheduleT()
    float_schedule_q.effectiveDate = "2025-01-17"
    float_schedule_q.terminationDate = "2030-01-17"
    float_schedule_q.calendar = Calendar.TARGET
    float_schedule_q.frequency = Frequency.Semiannual
    float_schedule_q.convention = BusinessDayConvention.ModifiedFollowing
    float_schedule_q.terminationDateConvention = BusinessDayConvention.ModifiedFollowing
    float_schedule_q.dateGenerationRule = DateGenerationRule.Forward
    
    float_leg = SwapFloatingLegT()
    float_leg.notional = notional
    float_leg.schedule = float_schedule_q
    float_leg.index = build_quantra_index_6m()
    float_leg.dayCounter = DayCounter.Actual360
    float_leg.spread = 0.0
    float_leg.paymentConvention = BusinessDayConvention.ModifiedFollowing
    
    swap = VanillaSwapT()
    swap.swapType = SwapType.Payer
    swap.fixedLeg = fixed_leg
    swap.floatingLeg = float_leg
    
    price_swap = PriceVanillaSwapT()
    price_swap.vanillaSwap = swap
    price_swap.discountingCurve = "discount"
    price_swap.forwardingCurve = "discount"
    
    request = PriceVanillaSwapRequestT()
    request.pricing = build_quantra_pricing([build_quantra_curve("discount")], indices=[build_index_def_eur6m()])
    request.swaps = [price_swap]
    
    response = client.price_vanilla_swaps(request)
    result = response.Swaps(0)
    q_npv = result.Npv()
    q_fair_rate = result.FairRate()
    
    # --- Compare ---
    npv_diff = abs(ql_npv - q_npv)
    rate_diff = abs(ql_fair_rate - q_fair_rate)
    
    print(f"  QuantLib NPV:       {ql_npv:>12.2f}")
    print(f"  Quantra NPV:        {q_npv:>12.2f}")
    print(f"  NPV Difference:     {npv_diff:>12.4f}")
    print(f"  QuantLib Fair Rate: {ql_fair_rate * 100:>11.4f}%")
    print(f"  Quantra Fair Rate:  {q_fair_rate * 100:>11.4f}%")
    print(f"  Rate Difference:    {rate_diff * 10000:>11.4f} bps")
    
    passed = npv_diff < TOLERANCE and rate_diff < 1e-6
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
    
    Swaption: 1Y into 5Y payer, 3.5% strike, 20% vol, 1M notional
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
    
    fixed_leg = SwapFixedLegT()
    fixed_leg.notional = notional
    fixed_leg.schedule = fixed_schedule_q
    fixed_leg.rate = strike
    fixed_leg.dayCounter = DayCounter.Thirty360
    fixed_leg.paymentConvention = BusinessDayConvention.ModifiedFollowing
    
    float_schedule_q = ScheduleT()
    float_schedule_q.effectiveDate = "2026-01-17"
    float_schedule_q.terminationDate = "2031-01-17"
    float_schedule_q.calendar = Calendar.TARGET
    float_schedule_q.frequency = Frequency.Semiannual
    float_schedule_q.convention = BusinessDayConvention.ModifiedFollowing
    float_schedule_q.terminationDateConvention = BusinessDayConvention.ModifiedFollowing
    float_schedule_q.dateGenerationRule = DateGenerationRule.Forward
    
    float_leg = SwapFloatingLegT()
    float_leg.notional = notional
    float_leg.schedule = float_schedule_q
    float_leg.index = build_quantra_index_6m()
    float_leg.dayCounter = DayCounter.Actual360
    float_leg.spread = 0.0
    float_leg.paymentConvention = BusinessDayConvention.ModifiedFollowing
    
    underlying_swap = VanillaSwapT()
    underlying_swap.swapType = SwapType.Payer
    underlying_swap.fixedLeg = fixed_leg
    underlying_swap.floatingLeg = float_leg
    
    swaption = SwaptionT()
    swaption.underlyingSwap = underlying_swap
    swaption.exerciseDate = "2026-01-15"
    swaption.exerciseType = ExerciseType.European
    swaption.settlementType = SettlementType.Physical
    
    price_swaption = PriceSwaptionT()
    price_swaption.swaption = swaption
    price_swaption.discountingCurve = "discount"
    price_swaption.forwardingCurve = "discount"
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
    
    cds = CDST()
    cds.side = ProtectionSide.Buyer
    cds.notional = notional
    cds.runningCoupon = spread
    cds.schedule = schedule_q
    cds.dayCounter = DayCounter.Actual360
    cds.businessDayConvention = BusinessDayConvention.Following
    
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
    
    # Build Quantra request using new schema with structured Tenor
    from quantra.BootstrapCurvesRequest import BootstrapCurvesRequestT
    from quantra.BootstrapCurveSpec import BootstrapCurveSpecT
    from quantra.CurveQuery import CurveQueryT
    from quantra.CurveGridSpec import CurveGridSpecT
    from quantra.TenorGrid import TenorGridT
    from quantra.Tenor import TenorT
    from quantra.CurveGrid import CurveGrid
    from quantra.CurveMeasure import CurveMeasure
    
    # Build structured tenors (new format: {n, unit})
    tenors = []
    for n, unit in [(3, TimeUnit.Months), (6, TimeUnit.Months), (1, TimeUnit.Years), 
                    (2, TimeUnit.Years), (5, TimeUnit.Years)]:
        tenor = TenorT()
        tenor.n = n
        tenor.unit = unit
        tenors.append(tenor)
    
    # Build tenor grid
    tenor_grid = TenorGridT()
    tenor_grid.tenors = tenors
    
    # Build grid spec
    grid_spec = CurveGridSpecT()
    grid_spec.gridType = CurveGrid.TenorGrid
    grid_spec.grid = tenor_grid
    
    # Build query
    query = CurveQueryT()
    query.measures = [CurveMeasure.DF]
    query.grid = grid_spec
    
    # Build curve spec
    curve_spec = BootstrapCurveSpecT()
    curve_spec.curve = build_quantra_curve("test_curve")
    curve_spec.query = query
    
    # Build request
    request = BootstrapCurvesRequestT()
    request.asOfDate = EVAL_DATE_STR
    request.indices = [build_index_def_eur6m()]
    request.curves = [curve_spec]
    
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
        ("FRA", lambda: test_fra(client, curve_handle)),
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
    print(f"Total: {passed_count}/{total} tests passed")
    
    return passed_count == total


if __name__ == "__main__":
    success = main()
    exit(0 if success else 1)