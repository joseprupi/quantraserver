"""
Quantra Python Client

A high-performance Python client for the Quantra distributed QuantLib pricing engine.
Uses FlatBuffers Object API for serialization and gRPC for transport.

Requests are built with the generated FlatBuffers Object API classes from the
`quantra` package (flatbuffers/python/), then passed to a Client method.

Example:
    from quantra_client import Client
    from quantra.PriceFixedRateBondRequest import PriceFixedRateBondRequestT

    request = PriceFixedRateBondRequestT()
    # ... populate request.pricing and request.bonds ...

    client = Client("localhost:50051")
    response = client.price_fixed_rate_bonds(request)
    print(f"NPV: {response.Bonds(0).Npv()}")

See examples/price_bonds.py for a complete, runnable request.
"""

import grpc
import flatbuffers
from typing import Union, Optional

# =============================================================================
# Request Types (Object API - the "T" classes with .Pack() method)
# =============================================================================
from quantra.PriceFixedRateBondRequest import PriceFixedRateBondRequestT
from quantra.PriceFloatingRateBondRequest import PriceFloatingRateBondRequestT
from quantra.PriceVanillaSwapRequest import PriceVanillaSwapRequestT
from quantra.PriceOisSwapRequest import PriceOisSwapRequestT
from quantra.PriceBasisSwapRequest import PriceBasisSwapRequestT
from quantra.PriceFRARequest import PriceFRARequestT
from quantra.PriceFxForwardRequest import PriceFxForwardRequestT
from quantra.PriceCrossCurrencySwapRequest import PriceCrossCurrencySwapRequestT
from quantra.PriceStructuredNoteRequest import PriceStructuredNoteRequestT
from quantra.PriceCapFloorRequest import PriceCapFloorRequestT
from quantra.PriceSwaptionRequest import PriceSwaptionRequestT
from quantra.PriceCDSRequest import PriceCDSRequestT
from quantra.BootstrapCurvesRequest import BootstrapCurvesRequestT
from quantra.CalibrateEquityVolRequest import CalibrateEquityVolRequestT
from quantra.BootstrapInflationCurvesRequest import BootstrapInflationCurvesRequestT
from quantra.Pricing import PricingT
from quantra.RatesMarketData import RatesMarketDataT
from quantra.CreditMarketData import CreditMarketDataT
from quantra.VolatilityMarketData import VolatilityMarketDataT
from quantra.EquityMarketData import EquityMarketDataT
from quantra.InflationMarketData import InflationMarketDataT
from quantra.PricingOptions import PricingOptionsT

# =============================================================================
# Response Types (Reader classes - for deserializing responses)
# =============================================================================
from quantra.PriceFixedRateBondResponse import PriceFixedRateBondResponse
from quantra.PriceFloatingRateBondResponse import PriceFloatingRateBondResponse
from quantra.PriceVanillaSwapResponse import PriceVanillaSwapResponse
from quantra.PriceOisSwapResponse import PriceOisSwapResponse
from quantra.PriceBasisSwapResponse import PriceBasisSwapResponse
from quantra.PriceFRAResponse import PriceFRAResponse
from quantra.PriceFxForwardResponse import PriceFxForwardResponse
from quantra.PriceCrossCurrencySwapResponse import PriceCrossCurrencySwapResponse
from quantra.PriceStructuredNoteResponse import PriceStructuredNoteResponse
from quantra.PriceCapFloorResponse import PriceCapFloorResponse
from quantra.PriceSwaptionResponse import PriceSwaptionResponse
from quantra.PriceCDSResponse import PriceCDSResponse
from quantra.BootstrapCurvesResponse import BootstrapCurvesResponse
from quantra.CalibrateEquityVolResponse import CalibrateEquityVolResponse
from quantra.BootstrapInflationCurvesResponse import BootstrapInflationCurvesResponse


class Client:
    """
    Quantra gRPC Client
    
    Connects to a Quantra pricing server and provides methods for pricing
    various financial instruments.
    
    Example:
        client = Client("localhost:50051")
        response = client.price_fixed_rate_bonds(request)
        for i in range(response.BondsLength()):
            bond = response.Bonds(i)
            print(f"Bond {i}: NPV = {bond.Npv()}")
    """
    
    # gRPC method paths
    METHODS = {
        'fixed_rate_bond': '/quantra.QuantraServer/PriceFixedRateBond',
        'floating_rate_bond': '/quantra.QuantraServer/PriceFloatingRateBond',
        'vanilla_swap': '/quantra.QuantraServer/PriceVanillaSwap',
        'ois_swap': '/quantra.QuantraServer/PriceOisSwap',
        'basis_swap': '/quantra.QuantraServer/PriceBasisSwap',
        'fra': '/quantra.QuantraServer/PriceFRA',
        'fx_forward': '/quantra.QuantraServer/PriceFxForward',
        'cross_currency_swap': '/quantra.QuantraServer/PriceCrossCurrencySwap',
        'structured_note': '/quantra.QuantraServer/PriceStructuredNote',
        'cap_floor': '/quantra.QuantraServer/PriceCapFloor',
        'swaption': '/quantra.QuantraServer/PriceSwaption',
        'cds': '/quantra.QuantraServer/PriceCDS',
        'bootstrap_curves': '/quantra.QuantraServer/BootstrapCurves',
        'bootstrap_inflation_curves': '/quantra.QuantraServer/BootstrapInflationCurves',
        'calibrate_equity_vol': '/quantra.QuantraServer/CalibrateEquityVol',
    }
    
    def __init__(
        self,
        target: str = "localhost:50051",
        secure: bool = False,
        credentials: Optional[grpc.ChannelCredentials] = None,
        options: Optional[list] = None,
    ):
        """
        Initialize the Quantra client.
        
        Args:
            target: Server address in "host:port" format
            secure: Use TLS if True
            credentials: Optional gRPC credentials for secure connection
            options: Optional gRPC channel options
        """
        self.target = target
        
        # Default options for large messages
        default_options = [
            ('grpc.max_receive_message_length', 100 * 1024 * 1024),
            ('grpc.max_send_message_length', 100 * 1024 * 1024),
        ]
        channel_options = options or default_options
        
        if secure:
            creds = credentials or grpc.ssl_channel_credentials()
            self.channel = grpc.secure_channel(target, creds, options=channel_options)
        else:
            self.channel = grpc.insecure_channel(target, options=channel_options)
    
    def close(self):
        """Close the gRPC channel."""
        if self.channel:
            self.channel.close()
    
    def __enter__(self):
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False
    
    # =========================================================================
    # Pricing Methods
    # =========================================================================
    
    def price_fixed_rate_bonds(
        self,
        request: PriceFixedRateBondRequestT
    ) -> PriceFixedRateBondResponse:
        """Price fixed rate bonds."""
        return self._call(
            'fixed_rate_bond',
            request,
            PriceFixedRateBondResponse
        )
    
    def price_floating_rate_bonds(
        self,
        request: PriceFloatingRateBondRequestT
    ) -> PriceFloatingRateBondResponse:
        """Price floating rate bonds."""
        return self._call(
            'floating_rate_bond',
            request,
            PriceFloatingRateBondResponse
        )
    
    def price_vanilla_swaps(
        self,
        request: PriceVanillaSwapRequestT
    ) -> PriceVanillaSwapResponse:
        """Price vanilla interest rate swaps: a VanillaSwap is two SwapLeg
        entries, fixedLeg (FixedCoupons) and floatingLeg (IborCoupons or
        CmsCoupons), each paid or received, in one currency, with its
        discounting curve (and forwarding curve on the floating leg) on the
        leg. The fields every coupon kind shares - notional (or per-period
        notionals), schedule, dayCounter, paymentConvention and the optional
        paymentCalendar / paymentLag - sit on the SwapLegT itself; the
        coupons union holds only the kind-specific ones (rate; index and
        spread; ...). The response carries legs[0] (fixed) / legs[1]
        (floating)."""
        return self._call(
            'vanilla_swap',
            request,
            PriceVanillaSwapResponse
        )

    def price_ois_swaps(
        self,
        request: PriceOisSwapRequestT
    ) -> PriceOisSwapResponse:
        """Price overnight-indexed swaps: an OisSwap is two SwapLeg entries,
        fixedLeg (FixedCoupons) and overnightLeg (OvernightCoupons), each
        paid or received, in the index's currency, curves on the legs. Both
        legs state their paymentCalendar and paymentLag on the leg (required
        here, so both legs pay on the lagged dates); notional, schedule and
        paymentConvention sit on the leg too, the coupons union holds only
        the kind-specific fields. The response carries legs[0] (fixed) /
        legs[1] (overnight)."""
        return self._call(
            'ois_swap',
            request,
            PriceOisSwapResponse
        )

    def price_basis_swaps(
        self,
        request: PriceBasisSwapRequestT
    ) -> PriceBasisSwapResponse:
        """Price basis swaps: a BasisSwap is two floating SwapLeg entries,
        leg1 and leg2 (IborCoupons or OvernightCoupons), each paid or
        received, in one currency, curves on the legs. The response carries
        legs[0] / legs[1] and the fair spread of the leg carrying the
        request's spread."""
        return self._call(
            'basis_swap',
            request,
            PriceBasisSwapResponse
        )
    
    def price_fras(
        self,
        request: PriceFRARequestT
    ) -> PriceFRAResponse:
        """Price Forward Rate Agreements."""
        return self._call(
            'fra',
            request,
            PriceFRAResponse
        )
    
    def price_fx_forwards(
        self,
        request: PriceFxForwardRequestT
    ) -> PriceFxForwardResponse:
        """Price outright FX forwards (FX spot in pricing.fx + one discount
        curve per currency)."""
        return self._call(
            'fx_forward',
            request,
            PriceFxForwardResponse
        )
    
    def price_cross_currency_swaps(
        self,
        request: PriceCrossCurrencySwapRequestT
    ) -> PriceCrossCurrencySwapResponse:
        """Price cross-currency swaps: two SwapLeg entries (FixedCoupons /
        IborCoupons / OvernightCoupons), each paid or received, in two
        currencies with principal exchanges; FX spot in pricing.fx, one
        discounting curve per leg."""
        return self._call(
            'cross_currency_swap',
            request,
            PriceCrossCurrencySwapResponse
        )
    
    def price_structured_notes(
        self,
        request: PriceStructuredNoteRequestT
    ) -> PriceStructuredNoteResponse:
        """Price structured notes on equity-index baskets (autocallables,
        Phoenix / Athena coupons, capital barriers) by Monte Carlo: equity
        underlyings, Black vols, FX spots/vols for quanto underlyings and a
        correlation matrix in pricing; one result per note with its standard
        error, terms echo, seasoning resolution and model echo."""
        return self._call(
            'structured_note',
            request,
            PriceStructuredNoteResponse
        )
    
    def price_cap_floors(
        self,
        request: PriceCapFloorRequestT
    ) -> PriceCapFloorResponse:
        """Price interest rate caps and floors."""
        return self._call(
            'cap_floor',
            request,
            PriceCapFloorResponse
        )
    
    def price_swaptions(
        self,
        request: PriceSwaptionRequestT
    ) -> PriceSwaptionResponse:
        """Price swaptions. The underlying is a VanillaSwap or an OisSwap
        in the same SwapLeg shape as the swap endpoints: both legs name the
        same discounting curve, the floating leg its forwarding curve; the
        PriceSwaption wrapper carries only the volatility and model ids."""
        return self._call(
            'swaption',
            request,
            PriceSwaptionResponse
        )
    
    def price_cds(
        self,
        request: PriceCDSRequestT
    ) -> PriceCDSResponse:
        """Price Credit Default Swaps."""
        return self._call(
            'cds',
            request,
            PriceCDSResponse
        )
    
    def bootstrap_curves(
        self,
        request: BootstrapCurvesRequestT
    ) -> BootstrapCurvesResponse:
        """Bootstrap yield curves and return sampled measures (DF, zero rates, forward rates)."""
        return self._call(
            'bootstrap_curves',
            request,
            BootstrapCurvesResponse
        )

    def bootstrap_inflation_curves(
        self,
        request: BootstrapInflationCurvesRequestT
    ) -> BootstrapInflationCurvesResponse:
        """Bootstrap inflation curves and sample zero/YoY measures on a date grid."""
        return self._call(
            'bootstrap_inflation_curves',
            request,
            BootstrapInflationCurvesResponse
        )
    
    def calibrate_equity_vol(
        self,
        request: CalibrateEquityVolRequestT
    ) -> CalibrateEquityVolResponse:
        """Fit an SSVI surface (BlackVolSpec shape=SsviCalibrate) to option quotes and
        return the calibrated slices with their forwards plus fit diagnostics."""
        return self._call(
            'calibrate_equity_vol',
            request,
            CalibrateEquityVolResponse
        )

    # =========================================================================
    # Internal Methods
    # =========================================================================
    
    def _call(self, method: str, request, response_class):
        """
        Make a gRPC call.
        
        Args:
            method: Method name key from METHODS dict
            request: Request object with .Pack() method
            response_class: Response class with .GetRootAs() method
            
        Returns:
            Deserialized response object
        """
        # 1. Serialize: Pack the Object API request into FlatBuffer bytes
        builder = flatbuffers.Builder(1024)
        packed = request.Pack(builder)
        builder.Finish(packed)
        request_bytes = bytes(builder.Output())
        
        # 2. Call: Send bytes over gRPC
        unary_call = self.channel.unary_unary(
            self.METHODS[method],
            request_serializer=lambda x: x,
            response_deserializer=lambda x: x,
        )
        
        try:
            response_bytes = unary_call(request_bytes)
        except grpc.RpcError as e:
            raise QuantraError(f"gRPC error: {e.code()} - {e.details()}") from e
        
        # 3. Deserialize: Return the Reader (not unpacked - more efficient)
        return response_class.GetRootAs(response_bytes, 0)


class QuantraError(Exception):
    """Exception raised for Quantra client errors."""
    pass


# =============================================================================
# Convenience Exports
# =============================================================================

# Re-export commonly used types for building requests
from quantra.Pricing import PricingT
from quantra.TermStructure import TermStructureT
from quantra.Schedule import ScheduleT
from quantra.PointsWrapper import PointsWrapperT
from quantra.DepositHelper import DepositHelperT
from quantra.SwapHelper import SwapHelperT
from quantra.BondHelper import BondHelperT
from quantra.FRAHelper import FRAHelperT
from quantra.FutureHelper import FutureHelperT
from quantra.Yield import YieldT
from quantra.IndexDef import IndexDefT
from quantra.IndexRef import IndexRefT
from quantra.IndexType import IndexType
from quantra.Fixing import FixingT

# NEW: Vol Surface types (typed by product family)
from quantra.VolSurfaceSpec import VolSurfaceSpecT
from quantra.OptionletVolSpec import OptionletVolSpecT
from quantra.SwaptionVolSpec import SwaptionVolSpecT
from quantra.SwaptionVolConstantSpec import SwaptionVolConstantSpecT
from quantra.SwaptionVolAtmMatrixSpec import SwaptionVolAtmMatrixSpecT
from quantra.SwaptionVolSmileCubeSpec import SwaptionVolSmileCubeSpecT
from quantra.SwaptionSabrParamsSpec import SwaptionSabrParamsSpecT
from quantra.SwaptionSabrCalibrateSpec import SwaptionSabrCalibrateSpecT
from quantra.SwaptionVolPayload import SwaptionVolPayload
from quantra.Period import PeriodT
from quantra.QuoteMatrix2D import QuoteMatrix2DT
from quantra.QuoteTensor3D import QuoteTensor3DT
from quantra.BlackVolSpec import BlackVolSpecT
from quantra.IrVolBaseSpec import IrVolBaseSpecT
from quantra.BlackVolBaseSpec import BlackVolBaseSpecT
from quantra.VolPayload import VolPayload

# NEW: Model types
from quantra.ModelSpec import ModelSpecT
from quantra.CapFloorModelSpec import CapFloorModelSpecT
from quantra.SwaptionModelSpec import SwaptionModelSpecT
from quantra.EquityVanillaModelSpec import EquityVanillaModelSpecT
from quantra.ModelPayload import ModelPayload

# NEW: Quote types
from quantra.QuoteSpec import QuoteSpecT

# Bonds
from quantra.FixedRateBond import FixedRateBondT
from quantra.PriceFixedRateBond import PriceFixedRateBondT
from quantra.FloatingRateBond import FloatingRateBondT
from quantra.PriceFloatingRateBond import PriceFloatingRateBondT

# Swaps (SwapLeg model: every leg is a SwapLegT whose couponsType /
# coupons union is one of FixedCouponsT, IborCouponsT, OvernightCouponsT,
# CmsCouponsT; results come back as SwapLegResult per leg)
from quantra.SwapLeg import SwapLegT
from quantra.SwapLegResult import SwapLegResultT
from quantra.SwapCoupons import SwapCoupons
from quantra.FixedCoupons import FixedCouponsT
from quantra.IborCoupons import IborCouponsT
from quantra.OvernightCoupons import OvernightCouponsT
from quantra.CmsCoupons import CmsCouponsT
from quantra.VanillaSwap import VanillaSwapT
from quantra.PriceVanillaSwap import PriceVanillaSwapT
from quantra.OisSwap import OisSwapT
from quantra.PriceOisSwap import PriceOisSwapT
from quantra.BasisSwap import BasisSwapT
from quantra.PriceBasisSwap import PriceBasisSwapT

# FRA
from quantra.FRA import FRAT
from quantra.PriceFRA import PriceFRAT

# FX market data and FX forward
from quantra.FxSpotSpec import FxSpotSpecT
from quantra.FxMarketData import FxMarketDataT
from quantra.FxForward import FxForwardT
from quantra.PriceFxForward import PriceFxForwardT

# Cross-currency swap (two SwapLegT entries in two currencies)
from quantra.CrossCurrencySwap import CrossCurrencySwapT
from quantra.PriceCrossCurrencySwap import PriceCrossCurrencySwapT

# Structured note (basket autocallables; correlation market data)
from quantra.StructuredNote import StructuredNoteT
from quantra.BasketUnderlying import BasketUnderlyingT
from quantra.QuantoSpec import QuantoSpecT
from quantra.ObservationEvent import ObservationEventT
from quantra.CouponRule import CouponRuleT
from quantra.CapitalProtection import CapitalProtectionT
from quantra.UpsideParticipation import UpsideParticipationT
from quantra.MonteCarloSpec import MonteCarloSpecT
from quantra.CorrelationMatrixSpec import CorrelationMatrixSpecT
from quantra.CorrelationMarketData import CorrelationMarketDataT

# Cap/Floor
from quantra.CapFloor import CapFloorT
from quantra.PriceCapFloor import PriceCapFloorT

# Swaption
from quantra.Swaption import SwaptionT
from quantra.PriceSwaption import PriceSwaptionT

# CDS
from quantra.CDS import CDST
from quantra.PriceCDS import PriceCDST
from quantra.CreditCurveSpec import CreditCurveSpecT
from quantra.CdsHelperConventions import CdsHelperConventionsT
from quantra.CdsQuote import CdsQuoteT
from quantra.CdsModelSpec import CdsModelSpecT

# Enums
from quantra.enums.DayCounter import DayCounter
from quantra.enums.Calendar import Calendar
from quantra.enums.BusinessDayConvention import BusinessDayConvention
from quantra.enums.Frequency import Frequency
from quantra.enums.TimeUnit import TimeUnit
from quantra.enums.DateGenerationRule import DateGenerationRule
from quantra.enums.Interpolator import Interpolator
from quantra.enums.BootstrapTrait import BootstrapTrait
from quantra.enums.Compounding import Compounding
from quantra.enums.RateAveragingType import RateAveragingType
from quantra.enums.SwapType import SwapType
from quantra.enums.PayReceive import PayReceive
from quantra.enums.FRAType import FRAType
from quantra.enums.FxDirection import FxDirection
from quantra.enums.SwapFlowKind import SwapFlowKind
from quantra.enums.CapFloorType import CapFloorType
from quantra.enums.ExerciseType import ExerciseType
from quantra.enums.SettlementType import SettlementType
from quantra.enums.ProtectionSide import ProtectionSide
from quantra.enums.VolatilityType import VolatilityType
from quantra.enums.IrModelType import IrModelType
from quantra.enums.EquityModelType import EquityModelType
from quantra.enums.CdsEngineType import CdsEngineType
from quantra.enums.CdsHelperModel import CdsHelperModel
from quantra.Point import Point  # Union type discriminator


__all__ = [
    # Client
    'Client',
    'QuantraError',
    
    # Request types
    'PriceFixedRateBondRequestT',
    'PriceFloatingRateBondRequestT',
    'PriceVanillaSwapRequestT',
    'PriceOisSwapRequestT',
    'PriceBasisSwapRequestT',
    'PriceFRARequestT',
    'PriceFxForwardRequestT',
    'PriceCrossCurrencySwapRequestT',
    'PriceStructuredNoteRequestT',
    'PriceCapFloorRequestT',
    'PriceSwaptionRequestT',
    'PriceCDSRequestT',
    'BootstrapCurvesRequestT',
    'BootstrapInflationCurvesRequestT',
    
    # Response types
    'PriceFixedRateBondResponse',
    'PriceFloatingRateBondResponse',
    'PriceVanillaSwapResponse',
    'PriceOisSwapResponse',
    'PriceBasisSwapResponse',
    'PriceFRAResponse',
    'PriceFxForwardResponse',
    'PriceCrossCurrencySwapResponse',
    'PriceStructuredNoteResponse',
    'PriceCapFloorResponse',
    'PriceSwaptionResponse',
    'PriceCDSResponse',
    'BootstrapCurvesResponse',
    'BootstrapInflationCurvesResponse',
    
    # Building blocks
    'PricingT',
    'RatesMarketDataT',
    'CreditMarketDataT',
    'VolatilityMarketDataT',
    'EquityMarketDataT',
    'InflationMarketDataT',
    'PricingOptionsT',
    'TermStructureT',
    'ScheduleT',
    'PointsWrapperT',
    'DepositHelperT',
    'SwapHelperT',
    'BondHelperT',
    'FRAHelperT',
    'FutureHelperT',
    'YieldT',
    'IndexDefT',
    'IndexRefT',
    'IndexType',
    'FixingT',
    
    # NEW: Vol Surface types
    'VolSurfaceSpecT',
    'OptionletVolSpecT',
    'SwaptionVolSpecT',
    'SwaptionVolConstantSpecT',
    'SwaptionVolAtmMatrixSpecT',
    'SwaptionVolSmileCubeSpecT',
    'SwaptionSabrParamsSpecT',
    'SwaptionSabrCalibrateSpecT',
    'SwaptionVolPayload',
    'PeriodT',
    'QuoteMatrix2DT',
    'QuoteTensor3DT',
    'BlackVolSpecT',
    'IrVolBaseSpecT',
    'BlackVolBaseSpecT',
    'VolPayload',
    
    # NEW: Model types
    'ModelSpecT',
    'CapFloorModelSpecT',
    'SwaptionModelSpecT',
    'EquityVanillaModelSpecT',
    'ModelPayload',
    
    # NEW: Quote types
    'QuoteSpecT',
    
    # Instruments
    'FixedRateBondT',
    'PriceFixedRateBondT',
    'FloatingRateBondT',
    'PriceFloatingRateBondT',
    'SwapLegT',
    'SwapLegResultT',
    'SwapCoupons',
    'FixedCouponsT',
    'IborCouponsT',
    'OvernightCouponsT',
    'CmsCouponsT',
    'VanillaSwapT',
    'PriceVanillaSwapT',
    'OisSwapT',
    'PriceOisSwapT',
    'BasisSwapT',
    'PriceBasisSwapT',
    'FRAT',
    'PriceFRAT',
    'FxSpotSpecT',
    'FxMarketDataT',
    'FxForwardT',
    'PriceFxForwardT',
    'CrossCurrencySwapT',
    'PriceCrossCurrencySwapT',
    'StructuredNoteT',
    'BasketUnderlyingT',
    'QuantoSpecT',
    'ObservationEventT',
    'CouponRuleT',
    'CapitalProtectionT',
    'UpsideParticipationT',
    'MonteCarloSpecT',
    'CorrelationMatrixSpecT',
    'CorrelationMarketDataT',
    'CapFloorT',
    'PriceCapFloorT',
    'SwaptionT',
    'PriceSwaptionT',
    'CDST',
    'PriceCDST',
    'CreditCurveSpecT',
    'CdsHelperConventionsT',
    'CdsQuoteT',
    'CdsModelSpecT',
    
    # Enums
    'DayCounter',
    'Calendar',
    'BusinessDayConvention',
    'Frequency',
    'TimeUnit',
    'DateGenerationRule',
    'Interpolator',
    'BootstrapTrait',
    'Compounding',
    'RateAveragingType',
    'SwapType',
    'PayReceive',
    'FRAType',
    'FxDirection',
    'SwapFlowKind',
    'CapFloorType',
    'ExerciseType',
    'SettlementType',
    'ProtectionSide',
    'VolatilityType',
    'IrModelType',
    'EquityModelType',
    'CdsEngineType',
    'CdsHelperModel',
    'Point',
]