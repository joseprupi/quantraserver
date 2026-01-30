"""
Quantra gRPC Client

Async client for communicating with Quantra pricing servers.
"""

import asyncio
from typing import List, Optional, Union
from dataclasses import dataclass
import grpc
from grpc import aio

from .models import (
    PriceFixedRateBondRequest,
    PriceFixedRateBondResponse,
    PriceFloatingRateBondRequest,
    PriceFloatingRateBondResponse,
    FixedRateBond,
    FloatingRateBond,
    TermStructure,
    Pricing,
    PriceFixedRateBond,
    PriceFloatingRateBond,
)
from .serializers import (
    serialize_price_fixed_rate_bond_request,
    deserialize_price_fixed_rate_bond_response,
)


@dataclass
class ClientConfig:
    """Client configuration"""
    host: str = "localhost"
    port: int = 50051
    secure: bool = False
    max_message_size: int = 100 * 1024 * 1024  # 100MB
    timeout: float = 30.0
    num_workers: Optional[int] = None  # Auto-detect from server


class Client:
    """
    Async Quantra pricing client.
    
    Example:
        async with Client("localhost:50051") as client:
            results = await client.price_bonds(bonds, curves=[curve])
    """
    
    def __init__(
        self,
        address: str = "localhost:50051",
        secure: bool = False,
        max_message_size: int = 100 * 1024 * 1024,
    ):
        """
        Initialize the client.
        
        Args:
            address: Server address in "host:port" format
            secure: Use TLS encryption
            max_message_size: Maximum gRPC message size in bytes
        """
        self.address = address
        self.secure = secure
        self.max_message_size = max_message_size
        self._channel: Optional[aio.Channel] = None
        self._stub = None
        
    async def __aenter__(self):
        await self.connect()
        return self
        
    async def __aexit__(self, exc_type, exc_val, exc_tb):
        await self.close()
        
    async def connect(self):
        """Establish connection to the server"""
        options = [
            ('grpc.max_receive_message_length', self.max_message_size),
            ('grpc.max_send_message_length', self.max_message_size),
        ]
        
        if self.secure:
            credentials = grpc.ssl_channel_credentials()
            self._channel = aio.secure_channel(self.address, credentials, options=options)
        else:
            self._channel = aio.insecure_channel(self.address, options=options)
            
        # Import the generated gRPC stub
        # Note: This assumes the FlatBuffers gRPC code is available
        # We'll need to generate this or use a generic approach
        await self._channel.channel_ready()
        
    async def close(self):
        """Close the connection"""
        if self._channel:
            await self._channel.close()
            self._channel = None
            
    # =========================================================================
    # High-level API
    # =========================================================================
    
    async def price(
        self,
        products: List[Union[FixedRateBond, FloatingRateBond]],
        curves: List[TermStructure],
        as_of_date: str,
        settlement_date: Optional[str] = None,
        num_requests: int = 1,
    ) -> List[dict]:
        """
        Price a list of products.
        
        Automatically detects product types and routes to appropriate methods.
        
        Args:
            products: List of bonds/swaps/etc to price
            curves: List of yield curves
            as_of_date: Pricing date (YYYY/MM/DD format)
            settlement_date: Settlement date (defaults to as_of_date)
            num_requests: Number of parallel requests to split into
            
        Returns:
            List of pricing results
        """
        if settlement_date is None:
            settlement_date = as_of_date
            
        # Group by product type
        fixed_bonds = [p for p in products if isinstance(p, FixedRateBond)]
        floating_bonds = [p for p in products if isinstance(p, FloatingRateBond)]
        
        results = []
        
        if fixed_bonds:
            fixed_results = await self.price_fixed_rate_bonds(
                fixed_bonds, curves, as_of_date, settlement_date, num_requests
            )
            results.extend(fixed_results)
            
        if floating_bonds:
            floating_results = await self.price_floating_rate_bonds(
                floating_bonds, curves, as_of_date, settlement_date, num_requests
            )
            results.extend(floating_results)
            
        return results
    
    async def price_fixed_rate_bonds(
        self,
        bonds: List[FixedRateBond],
        curves: List[TermStructure],
        as_of_date: str,
        settlement_date: Optional[str] = None,
        num_requests: int = 1,
        discount_curve: str = "depos",
    ) -> List[dict]:
        """
        Price fixed rate bonds.
        
        Args:
            bonds: List of FixedRateBond objects
            curves: List of TermStructure objects
            as_of_date: Pricing date
            settlement_date: Settlement date
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
    
    async def price_floating_rate_bonds(
        self,
        bonds: List[FloatingRateBond],
        curves: List[TermStructure],
        as_of_date: str,
        settlement_date: Optional[str] = None,
        num_requests: int = 1,
        discount_curve: str = "depos",
        forecast_curve: str = "depos",
    ) -> List[dict]:
        """Price floating rate bonds."""
        # Similar implementation...
        raise NotImplementedError("Floating rate bonds not yet implemented")
    
    # =========================================================================
    # Request building helpers
    # =========================================================================
    
    def _split_fixed_rate_bond_requests(
        self,
        bonds: List[FixedRateBond],
        curves: List[TermStructure],
        as_of_date: str,
        settlement_date: str,
        discount_curve: str,
        num_requests: int,
    ) -> List[PriceFixedRateBondRequest]:
        """Split bonds into multiple requests for parallel processing."""
        
        if num_requests <= 0:
            num_requests = 1
            
        # Calculate bonds per request
        bonds_per_request = max(1, len(bonds) // num_requests)
        
        requests = []
        for i in range(num_requests):
            start = i * bonds_per_request
            if i == num_requests - 1:
                # Last request gets remaining bonds
                end = len(bonds)
            else:
                end = start + bonds_per_request
                
            if start >= len(bonds):
                break
                
            batch = bonds[start:end]
            
            # Build request
            pricing = Pricing(
                as_of_date=as_of_date,
                settlement_date=settlement_date,
                curves=curves,
            )
            
            price_bonds = [
                PriceFixedRateBond(
                    fixed_rate_bond=bond,
                    discounting_curve=discount_curve,
                )
                for bond in batch
            ]
            
            request = PriceFixedRateBondRequest(
                pricing=pricing,
                bonds=price_bonds,
            )
            
            requests.append(request)
            
        return requests
    
    # =========================================================================
    # Low-level gRPC calls
    # =========================================================================
    
    async def _call_price_fixed_rate_bond(
        self,
        request: PriceFixedRateBondRequest,
    ) -> List[dict]:
        """Make the actual gRPC call for fixed rate bond pricing."""
        
        # Serialize request to FlatBuffers
        request_bytes = serialize_price_fixed_rate_bond_request(request)
        
        # Make gRPC call
        # Note: This is a simplified version - actual implementation needs
        # the generated gRPC stub from FlatBuffers
        
        # For now, we'll use a raw call approach
        response_bytes = await self._raw_call(
            "/quantra.QuantraServer/PriceFixedRateBond",
            request_bytes,
        )
        
        # Deserialize response
        return deserialize_price_fixed_rate_bond_response(response_bytes)
    
    async def _raw_call(self, method: str, request_bytes: bytes) -> bytes:
        """Make a raw gRPC call with FlatBuffers payloads."""
        
        # This is a workaround since FlatBuffers gRPC for Python is tricky
        # We use the unary_unary call with raw bytes
        
        call = self._channel.unary_unary(
            method,
            request_serializer=lambda x: x,  # Already bytes
            response_deserializer=lambda x: x,  # Return raw bytes
        )
        
        response = await call(request_bytes)
        return response


# Synchronous wrapper for convenience
class SyncClient:
    """
    Synchronous wrapper around the async client.
    
    Example:
        client = SyncClient("localhost:50051")
        results = client.price_bonds(bonds, curves=[curve])
    """
    
    def __init__(self, *args, **kwargs):
        self._async_client = Client(*args, **kwargs)
        self._loop = asyncio.new_event_loop()
        
    def __enter__(self):
        self._loop.run_until_complete(self._async_client.connect())
        return self
        
    def __exit__(self, *args):
        self._loop.run_until_complete(self._async_client.close())
        self._loop.close()
        
    def price(self, *args, **kwargs):
        return self._loop.run_until_complete(
            self._async_client.price(*args, **kwargs)
        )
        
    def price_fixed_rate_bonds(self, *args, **kwargs):
        return self._loop.run_until_complete(
            self._async_client.price_fixed_rate_bonds(*args, **kwargs)
        )
