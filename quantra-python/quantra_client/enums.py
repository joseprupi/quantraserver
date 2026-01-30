"""Auto-generated enum definitions from FlatBuffers schema"""

from enum import IntEnum

class DayCounter(IntEnum):
    Actual360 = 0
    Actual365Fixed = 1
    Actual365NoLeap = 2
    ActualActual = 3
    ActualActualISMA = 4
    ActualActualBond = 5
    ActualActualISDA = 6
    ActualActualHistorical = 7
    ActualActual365 = 8
    ActualActualAFB = 9
    ActualActualEuro = 10
    Business252 = 11
    One = 12
    Simple = 13
    Thirty360 = 14

class Interpolator(IntEnum):
    BackwardFlat = 0
    ForwardFlat = 1
    Linear = 2
    LogCubic = 3
    LogLinear = 4

class BootstrapTrait(IntEnum):
    Discount = 0
    FwdRate = 1
    InterpolatedDiscount = 2
    InterpolatedFwd = 3
    InterpolatedZero = 4
    ZeroRate = 5

class TimeUnit(IntEnum):
    Days = 0
    Hours = 1
    Microseconds = 2
    Milliseconds = 3
    Minutes = 4
    Months = 5
    Seconds = 6
    Weeks = 7
    Years = 8

class Calendar(IntEnum):
    Argentina = 0
    Australia = 1
    BespokeCalendar = 2
    Brazil = 3
    Canada = 4
    China = 5
    CzechRepublic = 6
    Denmark = 7
    Finland = 8
    Germany = 9
    HongKong = 10
    Hungary = 11
    Iceland = 12
    India = 13
    Indonesia = 14
    Israel = 15
    Italy = 16
    Japan = 17
    Mexico = 18
    NewZealand = 19
    Norway = 20
    NullCalendar = 21
    Poland = 22
    Romania = 23
    Russia = 24
    SaudiArabia = 25
    Singapore = 26
    Slovakia = 27
    SouthAfrica = 28
    SouthKorea = 29
    Sweden = 30
    Switzerland = 31
    TARGET = 32
    Taiwan = 33
    Turkey = 34
    Ukraine = 35
    UnitedKingdom = 36
    UnitedStates = 37
    UnitedStatesGovernmentBond = 38
    UnitedStatesNERC = 39
    UnitedStatesNYSE = 40
    UnitedStatesSettlement = 41
    WeekendsOnly = 42

class BusinessDayConvention(IntEnum):
    Following = 0
    HalfMonthModifiedFollowing = 1
    ModifiedFollowing = 2
    ModifiedPreceding = 3
    Nearest = 4
    Preceding = 5
    Unadjusted = 6

class Frequency(IntEnum):
    Annual = 0
    Bimonthly = 1
    Biweekly = 2
    Daily = 3
    EveryFourthMonth = 4
    EveryFourthWeek = 5
    Monthly = 6
    NoFrequency = 7
    Once = 8
    OtherFrequency = 9
    Quarterly = 10
    Semiannual = 11
    Weekly = 12

class DateGenerationRule(IntEnum):
    Backward = 0
    CDS = 1
    Forward = 2
    OldCDS = 3
    ThirdWednesday = 4
    Twentieth = 5
    TwentiethIMM = 6
    Zero = 7

class Ibor(IntEnum):
    Euribor10M = 0
    Euribor11M = 1
    Euribor1M = 2
    Euribor1Y = 3
    Euribor2M = 4
    Euribor2W = 5
    Euribor365_10M = 6
    Euribor365_11M = 7
    Euribor365_1M = 8
    Euribor365_1Y = 9
    Euribor365_2M = 10
    Euribor365_2W = 11
    Euribor365_3M = 12
    Euribor365_3W = 13
    Euribor365_4M = 14
    Euribor365_5M = 15
    Euribor365_6M = 16
    Euribor365_7M = 17
    Euribor365_8M = 18
    Euribor365_9M = 19
    Euribor365_SW = 20
    Euribor3M = 21
    Euribor3W = 22
    Euribor4M = 23
    Euribor5M = 24
    Euribor6M = 25
    Euribor7M = 26
    Euribor8M = 27
    Euribor9M = 28
    EuriborSW = 29

class Compounding(IntEnum):
    Compounded = 0
    Continuous = 1
    Simple = 2
    SimpleThenCompounded = 3

class SwapType(IntEnum):
    Payer = 0
    Receiver = 1

class FRAType(IntEnum):
    Long = 0
    Short = 1

class CapFloorType(IntEnum):
    Cap = 0
    Floor = 1
    Collar = 2

class ExerciseType(IntEnum):
    European = 0
    Bermudan = 1
    American = 2

class SettlementType(IntEnum):
    Physical = 0
    Cash = 1

class ProtectionSide(IntEnum):
    Buyer = 0
    Seller = 1

class VolatilityType(IntEnum):
    ShiftedLognormal = 0
    Normal = 1
