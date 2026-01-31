"""
Quantra Python Client

Uses FlatBuffers-generated T classes directly for serialization.
"""

import asyncio
import sys
import os
from typing import List, Optional, Union
from dataclasses import dataclass
import flatbuffers
import grpc
from grpc import aio

# Add FlatBuffers Python path
FB_PYTHON_PATH = os.environ.get("QUANTRA_FB_PYTHON_PATH", "/workspace/flatbuffers/python")
if FB_PYTHON_PATH not in sys.path:
    sys.path.insert(0, FB_PYTHON_PATH)

# Import FlatBuffers generated classes
from quantra.PriceFixedRateBondRequest import (
    PriceFixedRateBondRequest,
    PriceFixedRateBondRequestT,
)
from quantra.PriceFixedRateBondResponse import PriceFixedRateBondResponse
from quantra.FixedRateBondResponse import FixedRateBondResponse
from quantra.PriceFixedRateBond import PriceFixedRateBondT
from quantra.FixedRateBond import FixedRateBondT
from quantra.FixedRateBondValues import FixedRateBondValues
from quantra.Pricing import PricingT
from quantra.TermStructure import TermStructureT
from quantra.Schedule import ScheduleT
from quantra.DepositHelper import DepositHelperT
from quantra.SwapHelper import SwapHelperT
from quantra.BondHelper import BondHelperT
from quantra.FRAHelper import FRAHelperT
from quantra.FutureHelper import FutureHelperT
from quantra.PointsWrapper import PointsWrapperT
from quantra.Point import Point

# Import enums
from quantra.enums.DayCounter import DayCounter
from quantra.enums.Calendar import Calendar
from quantra.enums.BusinessDayConvention import BusinessDayConvention
from quantra.enums.Frequency import Frequency
from quantra.enums.TimeUnit import TimeUnit
from quantra.enums.DateGenerationRule import DateGenerationRule
from quantra.enums.Interpolator import Interpolator
from quantra.enums.BootstrapTrait import BootstrapTrait
from quantra.enums.Compounding import Compounding


# Re-export T classes with simpler names
FixedRateBond = FixedRateBondT
Schedule = ScheduleT
TermStructure = TermStructureT
DepositHelper = DepositHelperT
SwapHelper = SwapHelperT
BondHelper = BondHelperT
FRAHelper = FRAHelperT
FutureHelper = FutureHelperT
Pricing = PricingT
PriceFixedRateBond = PriceFixedRateBondT
PointsWrapper = PointsWrapperT


def create_deposit_point(deposit: DepositHelperT) -> PointsWrapperT:
    """Wrap a DepositHelper in a PointsWrapper."""
    wrapper = PointsWrapperT()
    wrapper.pointType = Point.DepositHelper
    wrapper.point = deposit
    return wrapper


def create_swap_point(swap: SwapHelperT) -> PointsWrapperT:
    """Wrap a SwapHelper in a PointsWrapper."""
    wrapper = PointsWrapperT()
    wrapper.pointType = Point.SwapHelper
    wrapper.point = swap
    return wrapper


def create_bond_point(bond: BondHelperT) -> PointsWrapperT:
    """Wrap a BondHelper in a PointsWrapper."""
    wrapper = PointsWrapperT()
    wrapper.pointType = Point.BondHelper
    wrapper.point = bond
    return wrapper


def create_fra_point(fra: FRAHelperT) -> PointsWrapperT:
    """Wrap a FRAHelper in a PointsWrapper."""
    wrapper = PointsWrapperT()
    wrapper.pointType = Point.FRAHelper
    wrapper.point = fra
    return wrapper


def create_future_point(future: FutureHelperT) -> PointsWrapperT:
    """Wrap a FutureHelper in a PointsWrapper."""
    wrapper = PointsWrapperT()
    wrapper.pointType = Point.FutureHelper
    wrapper.point = future
    return wrapper


class Client:
    """
    Async Quantra pricing client.
    
    Example:
        async with Client("localhost:50051") as client:
            results = await client.price_fixed_rate_bonds(bonds, curves=[curve])
    """
    
    def __init__(
        self,
        address: str = "localhost:50051",
        secure: bool = False,
        max_message_size: int = 100 * 1024 * 1024,
    ):
        self.address = address
        self.secure = secure
        self.max_message_size = max_message_size
        self._channel: Optional[aio.Channel] = None
        
    async def __aenter__(self):
        await self.connect()
        return self
        
    async def __aexit__(self, exc_type, exc_val, exc_tb):
        await self.close()
        
    async def connect(self):
        """Establish connection to the server."""
        options = [
            ('grpc.max_receive_message_length', self.max_message_size),
            ('grpc.max_send_message_length', self.max_message_size),
        ]
        
        if self.secure:
            credentials = grpc.ssl_channel_credentials()
            self._channel = aio.secure_channel(self.address, credentials, options=options)
        else:
            self._channel = aio.insecure_channel(self.address, options=options)
            
        await self._channel.channel_ready()
        
    async def close(self):
        """Close the connection."""
        if self._channel:
            await self._channel.close()
            self._channel = None

    async def price_fixed_rate_bonds(
        self,
        bonds: List[FixedRateBondT],
        curves: List[TermStructureT],
        as_of_date: str,
        settlement_date: Optional[str] = None,
        num_requests: int = 1,
        discount_curve: str = "depos",
    ) -> List[dict]:
        """
        Price fixed rate bonds.
        
        Args:
            bonds: List of FixedRateBondT objects
            curves: List of TermStructureT objects  
            as_of_date: Pricing date (YYYY/MM/DD format)
            settlement_date: Settlement date (defaults to as_of_date)
            num_requests: Number of parallel requests
            discount_curve: ID of the discounting curve
            
        Returns:
            List of pricing results with NPV, duration, etc.
        """
        if settlement_date is None:
            settlement_date = as_of_date
            
        # Build requests
        requests = self._split_fixed_rate_bond_requests(
            bonds, curves, as_of_date, settlement_date, discount_curve, num_requests
        )
        
        # Execute in parallel
        tasks = [self._call_price_fixed_rate_bond(req) for req in requests]
        responses = await asyncio.gather(*tasks, return_exceptions=True)
        
        # Flatten results
        results = []
        for resp in responses:
            if isinstance(resp, Exception):
                results.append({"error": str(resp)})
            else:
                results.extend(resp)
                
        return results

    def _split_fixed_rate_bond_requests(
        self,
        bonds: List[FixedRateBondT],
        curves: List[TermStructureT],
        as_of_date: str,
        settlement_date: str,
        discount_curve: str,
        num_requests: int,
    ) -> List[PriceFixedRateBondRequestT]:
        """Split bonds into multiple requests for parallel processing."""
        
        if num_requests <= 0:
            num_requests = 1
            
        bonds_per_request = max(1, len(bonds) // num_requests)
        
        requests = []
        for i in range(num_requests):
            start = i * bonds_per_request
            if i == num_requests - 1:
                end = len(bonds)
            else:
                end = start + bonds_per_request
                
            if start >= len(bonds):
                break
                
            batch = bonds[start:end]
            
            # Build pricing
            pricing = PricingT()
            pricing.asOfDate = as_of_date
            pricing.settlementDate = settlement_date
            pricing.curves = curves
            pricing.bondPricingDetails = False
            pricing.bondPricingFlows = False
            
            # Build price bonds
            price_bonds = []
            for bond in batch:
                pb = PriceFixedRateBondT()
                pb.fixedRateBond = bond
                pb.discountingCurve = discount_curve
                price_bonds.append(pb)
            
            # Build request
            request = PriceFixedRateBondRequestT()
            request.pricing = pricing
            request.bonds = price_bonds
            
            requests.append(request)
            
        return requests

    async def _call_price_fixed_rate_bond(
        self,
        request: PriceFixedRateBondRequestT,
    ) -> List[dict]:
        """Make the actual gRPC call for fixed rate bond pricing."""
        
        # Serialize request using Pack()
        builder = flatbuffers.Builder(4096)
        request_offset = request.Pack(builder)
        builder.Finish(request_offset)
        request_bytes = bytes(builder.Output())
        
        # Make gRPC call
        response_bytes = await self._raw_call(
            "/quantra.QuantraServer/PriceFixedRateBond",
            request_bytes,
        )
        
        # Deserialize response
        response = PriceFixedRateBondResponse.GetRootAs(response_bytes, 0)
        
        results = []
        for i in range(response.BondsLength()):
            try:
                bond_result = response.Bonds(i)
                results.append({
                    "npv": bond_result.Npv(),
                    "clean_price": bond_result.CleanPrice(),
                    "dirty_price": bond_result.DirtyPrice(),
                    "accrued_amount": bond_result.AccruedAmount(),
                    "yield": bond_result.Yield_(),
                    "accrued_days": bond_result.AccruedDays(),
                    "duration": bond_result.MacaulayDuration(),
                    "modified_duration": bond_result.ModifiedDuration(),
                    "convexity": bond_result.Convexity(),
                    "bps": bond_result.Bps(),
                })
            except Exception as e:
                import traceback
                results.append({"error": str(e), "traceback": traceback.format_exc()})
        
        return results
    
    async def _raw_call(self, method: str, request_bytes: bytes) -> bytes:
        """Make a raw gRPC call with FlatBuffers payloads."""
        call = self._channel.unary_unary(
            method,
            request_serializer=lambda x: x,
            response_deserializer=lambda x: x,
        )
        response = await call(request_bytes)
        return response


# Synchronous wrapper
class SyncClient:
    """Synchronous wrapper around the async client."""
    
    def __init__(self, *args, **kwargs):
        self._async_client = Client(*args, **kwargs)
        self._loop = asyncio.new_event_loop()
        
    def __enter__(self):
        self._loop.run_until_complete(self._async_client.connect())
        return self
        
    def __exit__(self, *args):
        self._loop.run_until_complete(self._async_client.close())
        self._loop.close()
        
    def price_fixed_rate_bonds(self, *args, **kwargs):
        return self._loop.run_until_complete(
            self._async_client.price_fixed_rate_bonds(*args, **kwargs)
        )