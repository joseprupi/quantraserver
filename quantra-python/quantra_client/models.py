"""Auto-generated model definitions from FlatBuffers schema"""

from __future__ import annotations
from dataclasses import dataclass, field
from typing import List, Optional, Union

from .enums import *

@dataclass
class BlackIborCouponPricer:
    """Generated from quantra.BlackIborCouponPricer"""
    optionlet_volatility_structure: Optional[OptionletVolatilityStructure] = None

@dataclass
class BondHelper:
    """Generated from quantra.BondHelper"""
    rate: Optional[float] = None
    settlement_days: Optional[int] = None
    face_amount: Optional[float] = None
    schedule: Optional[Schedule] = None
    coupon_rate: Optional[float] = None
    day_counter: Optional[DayCounter] = None
    business_day_convention: Optional[BusinessDayConvention] = None
    redemption: Optional[float] = None
    issue_date: Optional[str] = None

@dataclass
class CDS:
    """Generated from quantra.CDS"""
    side: Optional[ProtectionSide] = None
    notional: Optional[float] = None
    spread: Optional[float] = None
    schedule: Optional[Schedule] = None
    upfront: Optional[float] = 0.0
    day_counter: Optional[DayCounter] = None
    business_day_convention: Optional[BusinessDayConvention] = None

@dataclass
class CDSValues:
    """Generated from quantra.CDSValues"""
    npv: Optional[float] = None
    fair_spread: Optional[float] = None
    fair_upfront: Optional[float] = None
    default_leg_npv: Optional[float] = None
    premium_leg_npv: Optional[float] = None
    error: Optional[Error] = None

@dataclass
class CapFloor:
    """Generated from quantra.CapFloor"""
    cap_floor_type: Optional[CapFloorType] = None
    notional: Optional[float] = None
    strike: Optional[float] = None
    schedule: Optional[Schedule] = None
    index: Optional[Index] = None
    day_counter: Optional[DayCounter] = None
    business_day_convention: Optional[BusinessDayConvention] = None

@dataclass
class CapFloorLet:
    """Generated from quantra.CapFloorLet"""
    payment_date: Optional[str] = None
    accrual_start_date: Optional[str] = None
    accrual_end_date: Optional[str] = None
    fixing_date: Optional[str] = None
    strike: Optional[float] = None
    forward_rate: Optional[float] = None
    discount: Optional[float] = None
    price: Optional[float] = None

@dataclass
class CapFloorResponse:
    """Generated from quantra.CapFloorResponse"""
    npv: Optional[float] = None
    atm_rate: Optional[float] = None
    implied_volatility: Optional[float] = None
    cap_floor_lets: List[CapFloorLet] = field(default_factory=list)

@dataclass
class ConstantOptionletVolatility:
    """Generated from quantra.ConstantOptionletVolatility"""
    settlement_days: Optional[int] = None
    calendar: Optional[Calendar] = None
    business_day_convention: Optional[BusinessDayConvention] = None
    volatility: Optional[float] = None
    day_counter: Optional[DayCounter] = None

@dataclass
class CouponPricer:
    """Generated from quantra.CouponPricer"""
    id: Optional[str] = None
    pricer: Optional[Pricer] = None

@dataclass
class CreditCurve:
    """Generated from quantra.CreditCurve"""
    recovery_rate: Optional[float] = 0.4
    quotes: List[CreditSpreadQuote] = field(default_factory=list)
    flat_hazard_rate: Optional[float] = None

@dataclass
class CreditSpreadQuote:
    """Generated from quantra.CreditSpreadQuote"""
    tenor_number: Optional[int] = None
    tenor_time_unit: Optional[TimeUnit] = None
    spread: Optional[float] = None

@dataclass
class DepositHelper:
    """Generated from quantra.DepositHelper"""
    rate: Optional[float] = None
    tenor_time_unit: Optional[TimeUnit] = None
    tenor_number: Optional[int] = None
    fixing_days: Optional[int] = None
    calendar: Optional[Calendar] = None
    business_day_convention: Optional[BusinessDayConvention] = None
    day_counter: Optional[DayCounter] = None

@dataclass
class Error:
    """Generated from quantra.Error"""
    error_message: Optional[str] = None

@dataclass
class FRA:
    """Generated from quantra.FRA"""
    fra_type: Optional[FRAType] = None
    notional: Optional[float] = None
    start_date: Optional[str] = None
    maturity_date: Optional[str] = None
    strike: Optional[float] = None
    index: Optional[Index] = None
    day_counter: Optional[DayCounter] = None
    calendar: Optional[Calendar] = None
    business_day_convention: Optional[BusinessDayConvention] = None

@dataclass
class FRAHelper:
    """Generated from quantra.FRAHelper"""
    rate: Optional[float] = None
    months_to_start: Optional[int] = None
    months_to_end: Optional[int] = None
    fixing_days: Optional[int] = None
    calendar: Optional[Calendar] = None
    business_day_convention: Optional[BusinessDayConvention] = None
    day_counter: Optional[DayCounter] = None

@dataclass
class FRAResponse:
    """Generated from quantra.FRAResponse"""
    npv: Optional[float] = None
    forward_rate: Optional[float] = None
    spot_value: Optional[float] = None
    settlement_date: Optional[str] = None

@dataclass
class FixedRateBond:
    """Generated from quantra.FixedRateBond"""
    settlement_days: Optional[int] = None
    face_amount: Optional[float] = None
    rate: Optional[float] = None
    accrual_day_counter: Optional[DayCounter] = None
    payment_convention: Optional[BusinessDayConvention] = None
    redemption: Optional[float] = None
    issue_date: Optional[str] = None
    schedule: Optional[Schedule] = None

@dataclass
class FixedRateBondResponse:
    """Generated from quantra.FixedRateBondResponse"""
    npv: Optional[float] = None
    clean_price: Optional[float] = None
    dirty_price: Optional[float] = None
    accrued_amount: Optional[float] = None
    yield: Optional[float] = None
    accrued_days: Optional[float] = None
    macaulay_duration: Optional[float] = None
    modified_duration: Optional[float] = None
    convexity: Optional[float] = None
    bps: Optional[float] = None
    flows: List[FlowsWrapper] = field(default_factory=list)

@dataclass
class Fixing:
    """Generated from quantra.Fixing"""
    date: Optional[str] = None
    rate: Optional[float] = None

@dataclass
class FloatingRateBond:
    """Generated from quantra.FloatingRateBond"""
    settlement_days: Optional[int] = None
    face_amount: Optional[float] = None
    schedule: Optional[Schedule] = None
    index: Optional[Index] = None
    accrual_day_counter: Optional[DayCounter] = None
    payment_convention: Optional[BusinessDayConvention] = None
    fixing_days: Optional[int] = None
    spread: Optional[float] = None
    in_arrears: Optional[bool] = None
    redemption: Optional[float] = None
    issue_date: Optional[str] = None

@dataclass
class FloatingRateBondResponse:
    """Generated from quantra.FloatingRateBondResponse"""
    npv: Optional[float] = None
    clean_price: Optional[float] = None
    dirty_price: Optional[float] = None
    accrued_amount: Optional[float] = None
    yield: Optional[float] = None
    accrued_days: Optional[float] = None
    macaulay_duration: Optional[float] = None
    modified_duration: Optional[float] = None
    convexity: Optional[float] = None
    bps: Optional[float] = None
    flows: List[FlowsWrapper] = field(default_factory=list)

@dataclass
class FlowInterest:
    """Generated from quantra.FlowInterest"""
    amount: Optional[float] = None
    fixing_date: Optional[str] = None
    accrual_start_date: Optional[str] = None
    accrual_end_date: Optional[str] = None
    discount: Optional[float] = None
    rate: Optional[float] = None
    price: Optional[float] = None

@dataclass
class FlowInterestFloat:
    """Generated from quantra.FlowInterestFloat"""
    amount: Optional[float] = None
    fixing_date: Optional[str] = None
    accrual_start_date: Optional[str] = None
    accrual_end_date: Optional[str] = None
    discount: Optional[float] = None
    rate: Optional[float] = None
    price: Optional[float] = None

@dataclass
class FlowNotional:
    """Generated from quantra.FlowNotional"""
    date: Optional[str] = None
    amount: Optional[float] = None
    discount: Optional[float] = None
    price: Optional[float] = None

@dataclass
class FlowPastInterest:
    """Generated from quantra.FlowPastInterest"""
    amount: Optional[float] = None
    fixing_date: Optional[str] = None
    accrual_start_date: Optional[str] = None
    accrual_end_date: Optional[str] = None
    rate: Optional[float] = None

@dataclass
class FlowPastInterestFloat:
    """Generated from quantra.FlowPastInterestFloat"""
    amount: Optional[float] = None
    fixing_date: Optional[str] = None
    accrual_start_date: Optional[str] = None
    accrual_end_date: Optional[str] = None
    rate: Optional[float] = None

@dataclass
class FlowsWrapper:
    """Generated from quantra.FlowsWrapper"""
    flow: Optional[Flow] = None

@dataclass
class FutureHelper:
    """Generated from quantra.FutureHelper"""
    rate: Optional[float] = None
    future_start_date: Optional[str] = None
    future_months: Optional[int] = None
    calendar: Optional[Calendar] = None
    business_day_convention: Optional[BusinessDayConvention] = None
    day_counter: Optional[DayCounter] = None

@dataclass
class Index:
    """Generated from quantra.Index"""
    period_number: Optional[int] = None
    period_time_unit: Optional[TimeUnit] = None
    settlement_days: Optional[int] = None
    calendar: Optional[Calendar] = None
    business_day_convention: Optional[BusinessDayConvention] = None
    end_of_month: Optional[bool] = None
    day_counter: Optional[DayCounter] = None
    fixings: List[Fixing] = field(default_factory=list)

@dataclass
class PointsWrapper:
    """Generated from quantra.PointsWrapper"""
    point: Optional[Point] = None

@dataclass
class PriceCDS:
    """Generated from quantra.PriceCDS"""
    cds: Optional[CDS] = None
    discounting_curve: Optional[str] = None
    credit_curve: Optional[CreditCurve] = None

@dataclass
class PriceCDSRequest:
    """Generated from quantra.PriceCDSRequest"""
    pricing: Optional[Pricing] = None
    cds_list: List[PriceCDS] = field(default_factory=list)

@dataclass
class PriceCDSResponse:
    """Generated from quantra.PriceCDSResponse"""
    cds_list: List[CDSValues] = field(default_factory=list)

@dataclass
class PriceCapFloor:
    """Generated from quantra.PriceCapFloor"""
    cap_floor: Optional[CapFloor] = None
    discounting_curve: Optional[str] = None
    forwarding_curve: Optional[str] = None
    volatility: Optional[str] = None
    include_details: Optional[bool] = false

@dataclass
class PriceCapFloorRequest:
    """Generated from quantra.PriceCapFloorRequest"""
    pricing: Optional[Pricing] = None
    cap_floors: List[PriceCapFloor] = field(default_factory=list)

@dataclass
class PriceCapFloorResponse:
    """Generated from quantra.PriceCapFloorResponse"""
    cap_floors: List[CapFloorResponse] = field(default_factory=list)

@dataclass
class PriceFRA:
    """Generated from quantra.PriceFRA"""
    fra: Optional[FRA] = None
    discounting_curve: Optional[str] = None
    forwarding_curve: Optional[str] = None

@dataclass
class PriceFRARequest:
    """Generated from quantra.PriceFRARequest"""
    pricing: Optional[Pricing] = None
    fras: List[PriceFRA] = field(default_factory=list)

@dataclass
class PriceFRAResponse:
    """Generated from quantra.PriceFRAResponse"""
    fras: List[FRAResponse] = field(default_factory=list)

@dataclass
class PriceFixedRateBond:
    """Generated from quantra.PriceFixedRateBond"""
    fixed_rate_bond: Optional[FixedRateBond] = None
    discounting_curve: Optional[str] = None
    yield: Optional[Yield] = None

@dataclass
class PriceFixedRateBondRequest:
    """Generated from quantra.PriceFixedRateBondRequest"""
    pricing: Optional[Pricing] = None
    bonds: List[PriceFixedRateBond] = field(default_factory=list)

@dataclass
class PriceFixedRateBondResponse:
    """Generated from quantra.PriceFixedRateBondResponse"""
    bonds: List[FixedRateBondResponse] = field(default_factory=list)

@dataclass
class PriceFloatingRateBond:
    """Generated from quantra.PriceFloatingRateBond"""
    floating_rate_bond: Optional[FloatingRateBond] = None
    discounting_curve: Optional[str] = None
    forecasting_curve: Optional[str] = None
    coupon_pricer: Optional[str] = None
    yield: Optional[Yield] = None

@dataclass
class PriceFloatingRateBondRequest:
    """Generated from quantra.PriceFloatingRateBondRequest"""
    pricing: Optional[Pricing] = None
    bonds: List[PriceFloatingRateBond] = field(default_factory=list)

@dataclass
class PriceFloatingRateBondResponse:
    """Generated from quantra.PriceFloatingRateBondResponse"""
    bonds: List[FloatingRateBondResponse] = field(default_factory=list)

@dataclass
class PriceSwaption:
    """Generated from quantra.PriceSwaption"""
    swaption: Optional[Swaption] = None
    discounting_curve: Optional[str] = None
    forwarding_curve: Optional[str] = None
    volatility: Optional[str] = None

@dataclass
class PriceSwaptionRequest:
    """Generated from quantra.PriceSwaptionRequest"""
    pricing: Optional[Pricing] = None
    swaptions: List[PriceSwaption] = field(default_factory=list)

@dataclass
class PriceSwaptionResponse:
    """Generated from quantra.PriceSwaptionResponse"""
    swaptions: List[SwaptionResponse] = field(default_factory=list)

@dataclass
class PriceVanillaSwap:
    """Generated from quantra.PriceVanillaSwap"""
    vanilla_swap: Optional[VanillaSwap] = None
    discounting_curve: Optional[str] = None
    forwarding_curve: Optional[str] = None

@dataclass
class PriceVanillaSwapRequest:
    """Generated from quantra.PriceVanillaSwapRequest"""
    pricing: Optional[Pricing] = None
    swaps: List[PriceVanillaSwap] = field(default_factory=list)
    include_flows: Optional[bool] = false

@dataclass
class PriceVanillaSwapResponse:
    """Generated from quantra.PriceVanillaSwapResponse"""
    swaps: List[VanillaSwapResponse] = field(default_factory=list)

@dataclass
class Pricing:
    """Generated from quantra.Pricing"""
    as_of_date: str
    settlement_date: str
    curves: List[TermStructure]
    volatilities: List[VolatilityTermStructure] = field(default_factory=list)
    bond_pricing_details: Optional[bool] = false
    bond_pricing_flows: Optional[bool] = false
    coupon_pricers: List[CouponPricer] = field(default_factory=list)

@dataclass
class Schedule:
    """Generated from quantra.Schedule"""
    calendar: Optional[Calendar] = None
    effective_date: Optional[str] = None
    termination_date: Optional[str] = None
    frequency: Optional[Frequency] = None
    convention: Optional[BusinessDayConvention] = None
    termination_date_convention: Optional[BusinessDayConvention] = None
    date_generation_rule: Optional[DateGenerationRule] = None
    end_of_month: Optional[bool] = None

@dataclass
class SwapFixedLeg:
    """Generated from quantra.SwapFixedLeg"""
    schedule: Optional[Schedule] = None
    notional: Optional[float] = None
    rate: Optional[float] = None
    day_counter: Optional[DayCounter] = None
    payment_convention: Optional[BusinessDayConvention] = None

@dataclass
class SwapFloatingLeg:
    """Generated from quantra.SwapFloatingLeg"""
    schedule: Optional[Schedule] = None
    notional: Optional[float] = None
    index: Optional[Index] = None
    spread: Optional[float] = 0.0
    day_counter: Optional[DayCounter] = None
    payment_convention: Optional[BusinessDayConvention] = None
    fixing_days: Optional[int] = 2
    in_arrears: Optional[bool] = false

@dataclass
class SwapHelper:
    """Generated from quantra.SwapHelper"""
    rate: Optional[float] = None
    tenor_time_unit: Optional[TimeUnit] = None
    tenor_number: Optional[int] = None
    calendar: Optional[Calendar] = None
    sw_fixed_leg_frequency: Optional[Frequency] = None
    sw_fixed_leg_convention: Optional[BusinessDayConvention] = None
    sw_fixed_leg_day_counter: Optional[DayCounter] = None
    sw_floating_leg_index: Optional[Ibor] = None
    spread: Optional[float] = None
    fwd_start_days: Optional[int] = None

@dataclass
class SwapLegFlow:
    """Generated from quantra.SwapLegFlow"""
    payment_date: Optional[str] = None
    accrual_start_date: Optional[str] = None
    accrual_end_date: Optional[str] = None
    amount: Optional[float] = None
    discount: Optional[float] = None
    present_value: Optional[float] = None
    fixing_date: Optional[str] = None
    index_fixing: Optional[float] = None
    spread: Optional[float] = None
    rate: Optional[float] = None

@dataclass
class SwapLegResponse:
    """Generated from quantra.SwapLegResponse"""
    npv: Optional[float] = None
    bps: Optional[float] = None
    flows: List[SwapLegFlow] = field(default_factory=list)

@dataclass
class Swaption:
    """Generated from quantra.Swaption"""
    exercise_type: Optional[ExerciseType] = None
    settlement_type: Optional[SettlementType] = None
    exercise_date: Optional[str] = None
    underlying_swap: Optional[VanillaSwap] = None

@dataclass
class SwaptionResponse:
    """Generated from quantra.SwaptionResponse"""
    npv: Optional[float] = None
    implied_volatility: Optional[float] = None
    atm_forward: Optional[float] = None
    annuity: Optional[float] = None
    delta: Optional[float] = None
    vega: Optional[float] = None

@dataclass
class TermStructure:
    """Generated from quantra.TermStructure"""
    id: Optional[str] = None
    day_counter: Optional[DayCounter] = None
    interpolator: Optional[Interpolator] = None
    bootstrap_trait: Optional[BootstrapTrait] = None
    points: List[PointsWrapper] = field(default_factory=list)
    reference_date: Optional[str] = None

@dataclass
class VanillaSwap:
    """Generated from quantra.VanillaSwap"""
    swap_type: Optional[SwapType] = None
    fixed_leg: Optional[SwapFixedLeg] = None
    floating_leg: Optional[SwapFloatingLeg] = None

@dataclass
class VanillaSwapResponse:
    """Generated from quantra.VanillaSwapResponse"""
    npv: Optional[float] = None
    fair_rate: Optional[float] = None
    fair_spread: Optional[float] = None
    fixed_leg_bps: Optional[float] = None
    floating_leg_bps: Optional[float] = None
    fixed_leg_npv: Optional[float] = None
    floating_leg_npv: Optional[float] = None
    fixed_leg_flows: List[SwapLegFlow] = field(default_factory=list)
    floating_leg_flows: List[SwapLegFlow] = field(default_factory=list)

@dataclass
class VolatilityQuote:
    """Generated from quantra.VolatilityQuote"""
    tenor_number: Optional[int] = None
    tenor_time_unit: Optional[TimeUnit] = None
    strike: Optional[float] = None
    volatility: Optional[float] = None

@dataclass
class VolatilityTermStructure:
    """Generated from quantra.VolatilityTermStructure"""
    id: str
    reference_date: Optional[str] = None
    calendar: Optional[Calendar] = None
    business_day_convention: Optional[BusinessDayConvention] = None
    day_counter: Optional[DayCounter] = None
    volatility_type: Optional[VolatilityType] = None
    constant_vol: Optional[float] = None
    quotes: List[VolatilityQuote] = field(default_factory=list)

@dataclass
class Yield:
    """Generated from quantra.Yield"""
    day_counter: Optional[DayCounter] = None
    compounding: Optional[Compounding] = None
    frequency: Optional[Frequency] = None
