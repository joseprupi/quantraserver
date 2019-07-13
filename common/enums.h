//
// Created by Josep Rubio on 3/21/17.
//
#pragma once

#include <boost/assign/list_of.hpp>
#include <boost/assert.hpp>
#include <map>
#include <ql/quantlib.hpp>

using namespace QuantLib;
using namespace std;
using namespace boost::assign;

#ifndef CROWSAMPLE_ENUMS_H
#define CROWSAMPLE_ENUMS_H

#endif //CROWSAMPLE_ENUMS_H

enum BootstrapTrait {DiscountString, ZeroRateString, FwdRateString,
    InterpolatedFwdString, InterpolatedZeroString, InterpolatedDiscountString};

extern std::map<std::string, BootstrapTrait> BootstrapTraitString;

BootstrapTrait bootstrapTraitToQL(string bootstrapTrait);

enum PointType { FutureString, DepositString , FraString, SwapString, BondString, OISString, DatedOISString };

extern std::map<std::string, PointType> PointTypeString;

PointType pointTypeToQL(string pointType);

enum CalendarEnum { ArgentinaString, AustraliaString, BespokeCalendarString, BrazilString, CanadaString, ChinaString, CzechRepublicString, DenmarkString, FinlandString, GermanyString, HongKongString,
    HungaryString, IcelandString, IndiaString, IndonesiaString, IsraelString, ItalyString, JapanString, MexicoString, NewZealandString, NorwayString, NullCalendarString, PolandString,
    RomaniaString, RussiaString, SaudiArabiaString, SingaporeString, SlovakiaString, SouthAfricaString, SouthKoreaString, SwedenString, SwitzerlandString, TaiwanString,
    TARGETString, TurkeyString, UkraineString, UnitedKingdomString, UnitedStatesString, UnitedStatesSettlementString, UnitedStatesNYSEString, UnitedStatesGovernmentBondString, UnitedStatesNERCString, WeekendsOnlyString};

extern std::map<std::string, CalendarEnum> CalendarString;

Calendar calendarToQL(string calendar);

enum TimeUnitEnum { DaysString, WeeksString, MonthsString, YearsString, HoursString, MinutesString, SecondsString, MillisecondsString, MicrosecondsString };

extern std::map<std::string, TimeUnitEnum> TimeUnitString;

TimeUnit timeUnitToQL(string timeUnit);

enum BusinessDayConventionEnum { FollowingString, ModifiedFollowingString, PrecedingString, ModifiedPrecedingString, UnadjustedString, HalfMonthModifiedFollowingString, NearestString };

BusinessDayConvention conventionToQL(string dayConvention);

extern std::map<std::string, BusinessDayConventionEnum> BusinessDayConventionString;

enum DayCounterEnum { Actual360String, Actual365FixedString, Actual365NoLeapString, ActualActualString, ActualActualISMAString,
    ActualActualBondString, ActualActualISDAString, ActualActualHistoricalString, ActualActual365String, ActualActualAFBString,
    ActualActualEuroString, Business252String, OneString, SimpleString, Thirty360String, Thirty360BondBasisString };

DayCounter dayCounterToQL(string dayCounter);

extern std::map<std::string, DayCounterEnum> DayCounterString;

enum FrequencyEnum{NoFrequencyString, OnceString, AnnualString, SemiannualString, EveryFourthMonthString, QuarterlyString,
    BimonthlyString, MonthlyString, EveryFourthWeekString, BiweeklyString, WeeklyString, DailyString, OtherFrequencyString};

extern std::map<std::string, FrequencyEnum> FrequencyString;

Frequency frequencyToQL(string frequency);

enum IborEnum{EuriborSWString, Euribor2WString, Euribor3WString, Euribor1MString, Euribor2MString,
    Euribor3MString, Euribor4MString, Euribor5MString, Euribor6MString, Euribor7MString, Euribor8MString,
    Euribor9MString, Euribor10MString, Euribor11MString, Euribor1YString, Euribor365_SWString, Euribor365_2WString,
    Euribor365_3WString, Euribor365_1MString, Euribor365_2MString, Euribor365_3MString, Euribor365_4MString, Euribor365_5MString,
    Euribor365_6MString, Euribor365_7MString, Euribor365_8MString, Euribor365_9MString, Euribor365_10MString,
    Euribor365_11MString, Euribor365_1YString};

extern std::map<std::string, IborEnum> IborString;

boost::shared_ptr<IborIndex> iborToQL(string ibor);

enum OvernightIndexEnum{EoniaString, SoniaString};

extern std::map<std::string, OvernightIndexEnum> OvernightIndexString;

boost::shared_ptr<OvernightIndex> overnightIndexToQL(string overnightIndex);

enum CompoundingEnum{ SimpleCompoundingString, CompoundedString, ContinuousString, SimpleThenCompoundedString};

extern std::map<std::string, CompoundingEnum> CompoundingString;

Compounding compoundingToQL(string comp);

enum InterpolationEnum{ BackwardFlatString, ForwardFlatString, LinearString, LogLinearString, LogCubicString,
    BiLinearString, BiCubicString};

InterpolationEnum interpolationToQL(string comp);

extern std::map<std::string, InterpolationEnum> InterpolationString;

enum SwapTypeEnum{PayerString, ReceiverString};

extern std::map<std::string, SwapTypeEnum> SwapTypeString;

VanillaSwap::Type swapTypeToQL(string swapType);

bool validBusinessDate(string date, Calendar cal);

enum BoolEnum{True, False};

extern std::map<std::string, BoolEnum> BoolString;

bool boolToQL(string boolean);

enum DateGenerationEnum{ BackwardString, ForwardString, ZeroString, ThirdWednesdayString, TwentiethString,
    TwentiethIMMString, OldCDSString, CDSString};

extern std::map<std::string, DateGenerationEnum> DateGenerationString;

DateGeneration::Rule  dateGenerationToQL(string dateGeneration);

enum WeekDayEnum{ SundayString, MondayString, TuesdayString, WednesdayString, ThursdayString, FridayString, SaturdayString};

extern std::map<std::string, WeekDayEnum> WeekDayString;

Weekday  weekDayToQL(string weekDay);

enum MonthEnum{ JanuaryString, FebruaryString, MarchString, AprilString, MayString, JuneString, JulyString, AugustString,
    SeptemberString, OctoberString, NovemberString, DecemberString};

extern std::map<std::string, MonthEnum> MonthString;

Month monthToQL(string month);

enum ZeroSpreadTypeEnum {ParallelString, LinearInterpolatedString};

extern std::map<std::string, ZeroSpreadTypeEnum> ZeroSpreadTypeString;

ZeroSpreadTypeEnum ZeroSpreadTypeToQL(string ZeroSpreadType);

enum VolEnum{ BlackConstantVolString, BlackVarianceSurfaceString};

VolEnum volToQL(string comp);

enum OptionTypeEnum{PutString, CallString};

extern std::map<std::string, OptionTypeEnum> OptionTypeString;

Option::Type optionTypeToQL(string optionType);

enum OptionStyleEnum{EuropeanString};

extern std::map<std::string, OptionStyleEnum> OptionStyleString;

OptionStyleEnum OptionStyleToQL(string OptionStyle);