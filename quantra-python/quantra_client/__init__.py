"""
Quantra Python Client

A Python client for the Quantra distributed QuantLib pricing engine.

Example:
    from quantra_client import Client, FixedRateBond, Curve, DepositHelper
    
    client = Client("localhost:50051")
    
    curve = Curve(
        id="USD",
        points=[
            DepositHelper(rate=0.05, tenor_number=3, tenor_time_unit=TimeUnit.Months),
        ]
    )
    
    bond = FixedRateBond(
        face_amount=100,
        rate=0.045,
        issue_date="2020-01-15",
        ...
    )
    
    results = await client.price_bonds([bond], curves=[curve])
"""

from .enums import *
from .models import *
from .client import Client

__version__ = "0.1.0"
__all__ = [
    "Client",
    # Re-export commonly used models
    "FixedRateBond",
    "FloatingRateBond",
    "Schedule",
    "TermStructure",
    "DepositHelper",
    "SwapHelper",
    "Pricing",
    # Enums
    "DayCounter",
    "Calendar", 
    "BusinessDayConvention",
    "Frequency",
    "TimeUnit",
    "DateGenerationRule",
    "Interpolator",
    "BootstrapTrait",
]
