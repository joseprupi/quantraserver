"""
Quantra Python Client

A high-performance Python client for the Quantra distributed QuantLib pricing engine.
Uses FlatBuffers Object API for serialization and gRPC for transport.

Example:
    from quantra_client import Client
    from quantra_client.builders import (
        build_fixed_rate_bond_request,
        build_vanilla_swap_request,
    )
    
    client = Client("localhost:50051")
    
    request = build_fixed_rate_bond_request(...)
    response = client.price_fixed_rate_bonds(request)
    print(f"NPV: {response.Bonds(0).Npv()}")
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
from quantra.PriceFRARequest import PriceFRARequestT
from quantra.PriceCapFloorRequest import PriceCapFloorRequestT
from quantra.PriceSwaptionRequest import PriceSwaptionRequestT
from quantra.PriceCDSRequest import PriceCDSRequestT
from quantra.BootstrapCurvesRequest import BootstrapCurvesRequestT

# =============================================================================
# Response Types (Reader classes - for deserializing responses)
# =============================================================================
from quantra.PriceFixedRateBondResponse import PriceFixedRateBondResponse
from quantra.PriceFloatingRateBondResponse import PriceFloatingRateBondResponse
from quantra.PriceVanillaSwapResponse import PriceVanillaSwapResponse
from quantra.PriceFRAResponse import PriceFRAResponse
from quantra.PriceCapFloorResponse import PriceCapFloorResponse
from quantra.PriceSwaptionResponse import PriceSwaptionResponse
from quantra.PriceCDSResponse import PriceCDSResponse
from quantra.BootstrapCurvesResponse import BootstrapCurvesResponse


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
        'fra': '/quantra.QuantraServer/PriceFRA',
        'cap_floor': '/quantra.QuantraServer/PriceCapFloor',
        'swaption': '/quantra.QuantraServer/PriceSwaption',
        'cds': '/quantra.QuantraServer/PriceCDS',
        'bootstrap_curves': '/quantra.QuantraServer/BootstrapCurves',
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
        """Price vanilla interest rate swaps."""
        return self._call(
            'vanilla_swap',
            request,
            PriceVanillaSwapResponse
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
        """Price swaptions."""
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

# Swaps
from quantra.VanillaSwap import VanillaSwapT
from quantra.SwapFixedLeg import SwapFixedLegT
from quantra.SwapFloatingLeg import SwapFloatingLegT
from quantra.PriceVanillaSwap import PriceVanillaSwapT

# FRA
from quantra.FRA import FRAT
from quantra.PriceFRA import PriceFRAT

# Cap/Floor
from quantra.CapFloor import CapFloorT
from quantra.PriceCapFloor import PriceCapFloorT

# Swaption
from quantra.Swaption import SwaptionT
from quantra.PriceSwaption import PriceSwaptionT

# CDS
from quantra.CDS import CDST
from quantra.PriceCDS import PriceCDST
from quantra.CreditCurve import CreditCurveT

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
from quantra.enums.SwapType import SwapType
from quantra.enums.FRAType import FRAType
from quantra.enums.CapFloorType import CapFloorType
from quantra.enums.ExerciseType import ExerciseType
from quantra.enums.SettlementType import SettlementType
from quantra.enums.ProtectionSide import ProtectionSide
from quantra.enums.VolatilityType import VolatilityType
from quantra.enums.IrModelType import IrModelType
from quantra.enums.EquityModelType import EquityModelType
from quantra.Point import Point  # Union type discriminator


__all__ = [
    # Client
    'Client',
    'QuantraError',
    
    # Request types
    'PriceFixedRateBondRequestT',
    'PriceFloatingRateBondRequestT',
    'PriceVanillaSwapRequestT',
    'PriceFRARequestT',
    'PriceCapFloorRequestT',
    'PriceSwaptionRequestT',
    'PriceCDSRequestT',
    'BootstrapCurvesRequestT',
    
    # Response types
    'PriceFixedRateBondResponse',
    'PriceFloatingRateBondResponse',
    'PriceVanillaSwapResponse',
    'PriceFRAResponse',
    'PriceCapFloorResponse',
    'PriceSwaptionResponse',
    'PriceCDSResponse',
    'BootstrapCurvesResponse',
    
    # Building blocks
    'PricingT',
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
    'VanillaSwapT',
    'SwapFixedLegT',
    'SwapFloatingLegT',
    'PriceVanillaSwapT',
    'FRAT',
    'PriceFRAT',
    'CapFloorT',
    'PriceCapFloorT',
    'SwaptionT',
    'PriceSwaptionT',
    'CDST',
    'PriceCDST',
    'CreditCurveT',
    
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
    'SwapType',
    'FRAType',
    'CapFloorType',
    'ExerciseType',
    'SettlementType',
    'ProtectionSide',
    'VolatilityType',
    'IrModelType',
    'EquityModelType',
    'Point',
]