//
// Created by Josep Rubio on 3/23/17.
//

#include "enums.h"

std::map<std::string, BootstrapTrait> BootstrapTraitString = boost::assign::map_list_of
        ("Discount",DiscountString)
        ("ZeroRate",ZeroRateString)
        ("FwdRate",FwdRateString)
        ("InterpolatedFwd",InterpolatedFwdString)
        ("InterpolatedZero",InterpolatedZeroString)
        ("InterpolatedDiscount",InterpolatedDiscountString);

BootstrapTrait bootstrapTraitToQL(string bootstrapTrait){
    std::map<std::string, BootstrapTrait>::iterator it = BootstrapTraitString.find(bootstrapTrait);

    if(it != BootstrapTraitString.end()) {
        return it->second;
    }else{
        //throw std::runtime_error(pointType + " is not a valid PointType");
        QL_FAIL(bootstrapTrait + " is not a valid BootstrapTrait");
    }
}

std::map<std::string, PointType> PointTypeString = boost::assign::map_list_of
        ("Future", FutureString)
        ("Deposit", DepositString)
        ("Fra", FraString)
        ("Swap", SwapString)
        ("Bond", BondString)
        ("OIS", OISString)
        ("DatedOIS", DatedOISString);

PointType pointTypeToQL(string pointType){
    std::map<std::string, PointType>::iterator it = PointTypeString.find(pointType);

    if(it != PointTypeString.end()) {
        return it->second;
    }else{
        //throw std::runtime_error(pointType + " is not a valid PointType");
        QL_FAIL(pointType + " is not a valid PointType");
    }
}

std::map<std::string, CalendarEnum> CalendarString = map_list_of
        ("Argentina", ArgentinaString)
        ("Australia", AustraliaString)
        ("BespokeCalendar", BespokeCalendarString)
        ("Brazil", BrazilString)
        ("Canada",CanadaString)
        ("China",ChinaString)
        ("CzechRepublic",CzechRepublicString)
        ("Denmark",DenmarkString)
        ("Finland",FinlandString)
        ("Germany",GermanyString)
        ("HongKong",HongKongString)
        ("Hungary",HungaryString)
        ("Iceland",IcelandString)
        ("India",IndiaString)
        ("Indonesia",IndonesiaString)
        ("Israel",IsraelString)
        ("Italy",ItalyString)
        ("Japan",JapanString)
        ("Mexico",MexicoString)
        ("NewZealand",NewZealandString)
        ("Norway",NorwayString)
        ("NullCalendar",NullCalendarString)
        ("Poland",PolandString)
        ("Romania",RomaniaString)
        ("Russia",RussiaString)
        ("SaudiArabia",SaudiArabiaString)
        ("Singapore",SingaporeString)
        ("Slovakia",SlovakiaString)
        ("SouthAfrica",SouthAfricaString)
        ("SouthKorea",SouthKoreaString)
        ("Sweden",SwedenString)
        ("Switzerland",SwitzerlandString)
        ("Taiwan",TaiwanString)
        ("TARGET",TARGETString)
        ("Turkey",TurkeyString)
        ("Ukraine",UkraineString)
        ("UnitedKingdom",UnitedKingdomString)
        ("UnitedStates",UnitedStatesString)
        ("UnitedStatesSettlement",UnitedStatesSettlementString)
        ("UnitedStatesNYSE",UnitedStatesNYSEString)
        ("UnitedStatesGovernmentBond",UnitedStatesGovernmentBondString)
        ("UnitedStatesNERC",UnitedStatesNERCString)
        ("WeekendsOnly",WeekendsOnlyString);

Calendar calendarToQL(string calendar){

    std::map<std::string, CalendarEnum>::iterator it = CalendarString.find(calendar);

    if(it != CalendarString.end()) {

        CalendarEnum calendarEnum = it->second;

        switch (calendarEnum) {
            case ArgentinaString:
                return Argentina();
            case AustraliaString:
                return Australia();
            case BespokeCalendarString:
                return BespokeCalendar();
            case BrazilString:
                return Brazil();
            case CanadaString:
                return Canada();
            case ChinaString:
                return China();
            case CzechRepublicString:
                return CzechRepublic();
            case DenmarkString:
                return Denmark();
            case FinlandString:
                return Finland();
            case GermanyString:
                return Germany();
            case HongKongString:
                return HongKong();
            case HungaryString:
                return Hungary();
            case IcelandString:
                return Iceland();
            case IndiaString:
                return India();
            case IndonesiaString:
                return Indonesia();
            case IsraelString:
                return Israel();
            case ItalyString:
                return Italy();
            case JapanString:
                return Japan();
            case MexicoString:
                return Mexico();
            case NewZealandString:
                return NewZealand();
            case NorwayString:
                return Norway();
            case NullCalendarString:
                return NullCalendar();
            case PolandString:
                return Poland();
            case RomaniaString:
                return Romania();
            case RussiaString:
                return Russia();
            case SaudiArabiaString:
                return SaudiArabia();
            case SingaporeString:
                return Singapore();
            case SlovakiaString:
                return Slovakia();
            case SouthAfricaString:
                return SouthAfrica();
            case SouthKoreaString:
                return SouthKorea();
            case SwedenString:
                return Sweden();
            case SwitzerlandString:
                return Switzerland();
            case TaiwanString:
                return Taiwan();
            case TARGETString:
                return TARGET();
            case TurkeyString:
                return Turkey();
            case UkraineString:
                return Ukraine();
            case UnitedKingdomString:
                return UnitedKingdom();
            case UnitedStatesString:
                return UnitedStates();
            case UnitedStatesSettlementString:
                return UnitedStates(UnitedStates::Settlement);
            case UnitedStatesNYSEString:
                return UnitedStates(UnitedStates::NYSE);
            case UnitedStatesGovernmentBondString:
                return UnitedStates(UnitedStates::GovernmentBond);
            case UnitedStatesNERCString:
                return UnitedStates(UnitedStates::NERC);
            case WeekendsOnlyString:
                return WeekendsOnly();
        }
    }else{
        //throw std::runtime_error(calendar + " is not a valid calendar");
        QL_FAIL(calendar + " is not a valid calendar");
    }
}

std::map<std::string, TimeUnitEnum> TimeUnitString = map_list_of
        ("Days", DaysString)
        ("Weeks", WeeksString)
        ("Months",MonthsString)
        ("Years",YearsString)
        ("Hours",HoursString)
        ("Minutes",MinutesString)
        ("Seconds",SecondsString)
        ("Milliseconds",MillisecondsString)
        ("Microseconds",MicrosecondsString);

TimeUnit timeUnitToQL(string timeUnit){
    std::map<std::string, TimeUnitEnum>::iterator it = TimeUnitString.find(timeUnit);

    if(it != TimeUnitString.end()) {

        TimeUnitEnum tineUnitEnum = it->second;

        switch (tineUnitEnum) {
            case DaysString:
                return Days;
            case WeeksString:
                return Weeks;
            case MonthsString:
                return Months;
            case YearsString:
                return Years;
            case HoursString:
                return Hours;
            case MinutesString:
                return Minutes;
            case SecondsString:
                return Seconds;
            case MillisecondsString:
                return Milliseconds;
            case MicrosecondsString:
                return Microseconds;
        }
    }else{
        QL_FAIL(timeUnit + " is not a valid TimeUnit");
    }
}

std::map<std::string, BusinessDayConventionEnum> BusinessDayConventionString = map_list_of
        ("Following", FollowingString)
        ("ModifiedFollowing", ModifiedFollowingString)
        ("Preceding",PrecedingString)
        ("ModifiedPreceding",ModifiedPrecedingString)
        ("Unadjusted",UnadjustedString)
        ("HalfMonthModifiedFollowing",HalfMonthModifiedFollowingString)
        ("Nearest",NearestString);

BusinessDayConvention conventionToQL(string dayConvention){
    std::map<std::string, BusinessDayConventionEnum>::iterator it = BusinessDayConventionString.find(dayConvention);

    if(it != BusinessDayConventionString.end()) {

        BusinessDayConventionEnum conventionEnum = it->second;

        switch (conventionEnum) {
            case FollowingString:
                return Following;
            case ModifiedFollowingString:
                return ModifiedFollowing;
            case PrecedingString:
                return Preceding;
            case ModifiedPrecedingString:
                return ModifiedPreceding;
            case UnadjustedString:
                return Unadjusted;
            case HalfMonthModifiedFollowingString:
                return HalfMonthModifiedFollowing;
            case NearestString:
                return Nearest;
        }
    }else{
        //throw std::runtime_error(dayConvention + " is not a valid BusinessDayConvention");
        QL_FAIL(dayConvention + " is not a valid BusinessDayConvention");
    }
}

std::map<std::string, DayCounterEnum> DayCounterString = map_list_of
        ("Actual360", Actual360String)
        ("Actual365Fixed", Actual365FixedString)
        ("Actual365NoLeap",Actual365NoLeapString)
        ("ActualActual", ActualActualString)
        ("ActualActualISMA", ActualActualISMAString)
        ("ActualActualBond", ActualActualBondString)
        ("ActualActualISDA", ActualActualISDAString)
        ("ActualActualHistorical", ActualActualHistoricalString)
        ("ActualActual365", ActualActual365String)
        ("ActualActualAFB", ActualActualAFBString)
        ("ActualActualEuro", ActualActualEuroString)
        ("Business252", Business252String)
        ("One", OneString)
        ("Simple", SimpleString)
        ("Thirty360", Thirty360String);

DayCounter dayCounterToQL(string dayCounter){
    std::map<std::string, DayCounterEnum>::iterator it = DayCounterString.find(dayCounter);

    if(it != DayCounterString.end()) {

        DayCounterEnum dayCounterEnum = it->second;

        switch (dayCounterEnum) {
            case Actual360String:
                return Actual360();
            case Actual365FixedString:
                return Actual365Fixed();
            case Actual365NoLeapString:
                return Actual365NoLeap();
            case ActualActualString:
                return ActualActual();
            case ActualActualISMAString:
                return ActualActual(ActualActual::ISMA);
            case ActualActualBondString:
                return ActualActual(ActualActual::Bond);
            case ActualActualISDAString:
                return ActualActual(ActualActual::ISDA);
            case ActualActualHistoricalString:
                return ActualActual(ActualActual::Historical);
            case ActualActual365String:
                return ActualActual(ActualActual::Actual365);
            case ActualActualAFBString:
                return ActualActual(ActualActual::AFB);
            case ActualActualEuroString:
                return ActualActual(ActualActual::Euro);
            case Business252String:
                return Business252();
            case OneString:
                return OneDayCounter();
            case SimpleString:
                return SimpleDayCounter();
            case Thirty360String:
                return Thirty360();
        }
    }else{
        //throw std::runtime_error(dayCounter + " is not a valid DayCounter");
        QL_FAIL(dayCounter + " is not a valid DayCounter");
    }
}

std::map<std::string, FrequencyEnum> FrequencyString = map_list_of
        ("NoFrequency", NoFrequencyString)
        ("Once", OnceString)
        ("Annual", AnnualString)
        ("Semiannual",SemiannualString)
        ("EveryFourthMonth", EveryFourthMonthString)
        ("Quarterly", QuarterlyString)
        ("Bimonthly", BimonthlyString)
        ("Monthly", MonthlyString)
        ("EveryFourthWeek", EveryFourthWeekString)
        ("Biweekly", BiweeklyString)
        ("Weekly", WeeklyString)
        ("Daily", DailyString)
        ("OtherFrequency", OtherFrequencyString);

Frequency frequencyToQL(string frequency){
    std::map<std::string, FrequencyEnum>::iterator it = FrequencyString.find(frequency);

    if(it != FrequencyString.end()) {

        FrequencyEnum frequencyEnum = it->second;

        switch (frequencyEnum) {
            case NoFrequencyString:
                return NoFrequency;
            case OnceString:
                return Once;
            case AnnualString:
                return Annual;
            case SemiannualString:
                return Semiannual;
            case EveryFourthMonthString:
                return EveryFourthMonth;
            case QuarterlyString:
                return Quarterly;
            case BimonthlyString:
                return Bimonthly;
            case MonthlyString:
                return Monthly;
            case EveryFourthWeekString:
                return EveryFourthWeek;
            case BiweeklyString:
                return Biweekly;
            case WeeklyString:
                return Weekly;
            case DailyString:
                return Daily;
            case OtherFrequencyString:
                return OtherFrequency;
        }
    }else{
        //throw std::runtime_error(frequency + " is not a valid Frequency");
        QL_FAIL(frequency + " is not a valid Frequency");
    }
}

std::map<std::string, IborEnum> IborString = map_list_of
        ("EuriborSW", EuriborSWString)
        ("Euribor2W", Euribor2WString)
        ("Euribor3W", Euribor3WString)
        ("Euribor1M", Euribor1MString)
        ("Euribor2M", Euribor2MString)
        ("Euribor3M", Euribor3MString)
        ("Euribor4M", Euribor4MString)
        ("Euribor5M", Euribor5MString)
        ("Euribor6M", Euribor6MString)
        ("Euribor7M", Euribor7MString)
        ("Euribor8M", Euribor8MString)
        ("Euribor9M", Euribor9MString)
        ("Euribor10M", Euribor10MString)
        ("Euribor11M", Euribor11MString)
        ("Euribor1Y", Euribor1YString)

        ("Euribor365_SW", Euribor365_SWString)
        ("Euribor365_2W", Euribor365_2WString)
        ("Euribor365_3W", Euribor365_3WString)
        ("Euribor365_1M", Euribor365_1MString)
        ("Euribor365_2M", Euribor365_2MString)
        ("Euribor365_3M", Euribor365_3MString)
        ("Euribor365_4M", Euribor365_4MString)
        ("Euribor365_5M", Euribor365_5MString)
        ("Euribor365_6M", Euribor365_6MString)
        ("Euribor365_7M", Euribor365_7MString)
        ("Euribor365_8M", Euribor365_8MString)
        ("Euribor365_9M", Euribor365_9MString)
        ("Euribor365_10M", Euribor365_10MString)
        ("Euribor365_11M", Euribor365_11MString)
        ("Euribor365_1Y", Euribor365_1YString);

boost::shared_ptr<IborIndex> iborToQL(string ibor){
    std::map<std::string, IborEnum>::iterator it = IborString.find(ibor);

    if(it != IborString.end()) {

        IborEnum iborEnum = it->second;

        boost::shared_ptr<IborIndex> index;

        switch (iborEnum) {
            case EuriborSWString:
                index = boost::shared_ptr<IborIndex> (new EuriborSW());
                break;
            case Euribor2WString:
                index = boost::shared_ptr<IborIndex> (new Euribor2W());
                break;
            case Euribor3WString:
                index = boost::shared_ptr<IborIndex> (new Euribor3W());
                break;
            case Euribor1MString:
                index = boost::shared_ptr<IborIndex> (new Euribor1M());
                break;
            case Euribor2MString:
                index = boost::shared_ptr<IborIndex> (new Euribor2M());
                break;
            case Euribor3MString:
                index = boost::shared_ptr<IborIndex> (new Euribor3M());
                break;
            case Euribor4MString:
                index = boost::shared_ptr<IborIndex> (new Euribor4M());
                break;
            case Euribor5MString:
                index = boost::shared_ptr<IborIndex> (new Euribor5M());
                break;
            case Euribor6MString:
                index = boost::shared_ptr<IborIndex> (new Euribor6M());
                break;
            case Euribor7MString:
                index = boost::shared_ptr<IborIndex> (new Euribor7M());
                break;
            case Euribor8MString:
                index = boost::shared_ptr<IborIndex> (new Euribor8M());
                break;
            case Euribor9MString:
                index = boost::shared_ptr<IborIndex> (new Euribor9M());
                break;
            case Euribor10MString:
                index = boost::shared_ptr<IborIndex> (new Euribor10M());
                break;
            case Euribor11MString:
                index = boost::shared_ptr<IborIndex> (new Euribor11M());
                break;
            case Euribor1YString:
                index = boost::shared_ptr<IborIndex> (new Euribor1Y());
                break;
            case Euribor365_SWString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_SW());
                break;
            case Euribor365_2WString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_2W());
                break;
            case Euribor365_3WString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_3W());
                break;
            case Euribor365_1MString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_1M());
                break;
            case Euribor365_2MString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_2M());
                break;
            case Euribor365_3MString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_3M());
                break;
            case Euribor365_4MString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_4M());
                break;
            case Euribor365_5MString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_5M());
                break;
            case Euribor365_6MString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_6M());
                break;
            case Euribor365_7MString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_7M());
                break;
            case Euribor365_8MString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_8M());
                break;
            case Euribor365_9MString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_9M());
                break;
            case Euribor365_10MString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_10M());
                break;
            case Euribor365_11MString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_11M());
                break;
            case Euribor365_1YString:
                index = boost::shared_ptr<IborIndex> (new Euribor365_1Y());
                break;
        }
        return index;
    }else{
        //throw std::runtime_error(ibor + " is not a valid IborIndex");
        QL_FAIL(ibor + " is not a valid IborIndex");
    }
}

std::map<std::string, OvernightIndexEnum> OvernightIndexString = map_list_of
        ("Eonia", EoniaString)
        ("Sonia", SoniaString);

boost::shared_ptr<OvernightIndex> overnightIndexToQL(string overnightIndex){
    std::map<std::string, OvernightIndexEnum>::iterator it = OvernightIndexString.find(overnightIndex);

    if(it != OvernightIndexString.end()) {

        OvernightIndexEnum iborEnum = it->second;

        boost::shared_ptr<OvernightIndex> index;

        switch (iborEnum) {
            case EoniaString:
                index = boost::shared_ptr<OvernightIndex> (new Eonia());
                break;
            case SoniaString:
                index = boost::shared_ptr<OvernightIndex> (new Sonia());
                break;
        }
        return index;
    }else{
        //throw std::runtime_error(ibor + " is not a valid IborIndex");
        QL_FAIL(overnightIndex + " is not a valid OvernightIndex");
    }
}

std::map<std::string, CompoundingEnum> CompoundingString = map_list_of
        ("Simple", SimpleCompoundingString)
        ("Compounded", CompoundedString)
        ("Continuous", ContinuousString)
        ("SimpleThenCompounded",SimpleThenCompoundedString);

Compounding compoundingToQL(string comp){
    std::map<std::string, CompoundingEnum>::iterator it = CompoundingString.find(comp);

    if(it != CompoundingString.end()) {

        CompoundingEnum compoundingEnum = it->second;

        switch (compoundingEnum) {
            case SimpleCompoundingString:
                return Simple;
            case CompoundedString:
                return Compounded;
            case ContinuousString:
                return Continuous;
            case SimpleThenCompoundedString:
                return SimpleThenCompounded;
        }
    }else{
        //throw std::runtime_error(comp + " is not a valid Compounding");
        QL_FAIL(comp + " is not a valid Compounding");
    }
}

std::map<std::string, InterpolationEnum> InterpolationString = map_list_of
        ("BackwardFlat", BackwardFlatString)
        ("ForwardFlat", ForwardFlatString)
        ("Linear", LinearString)
        ("LogLinear",LogLinearString)
        ("LogCubic",LogCubicString)
        ("BiLinear",BiLinearString)
        ("BiCubic",BiCubicString);

InterpolationEnum interpolationToQL(string comp){
    std::map<std::string, InterpolationEnum>::iterator it = InterpolationString.find(comp);

    if(it != InterpolationString.end()) {
        return it->second;
    }else{
        //throw std::runtime_error(comp + " is not a valid Interpolation");
        QL_FAIL(comp + " is not a valid Interpolation");
    }
}

std::map<std::string, SwapTypeEnum> SwapTypeString = map_list_of
        ("Payer", PayerString)
        ("Receiver", ReceiverString);

VanillaSwap::Type swapTypeToQL(string swapType){
    std::map<std::string, SwapTypeEnum>::iterator it = SwapTypeString.find(swapType);

    if(it != SwapTypeString.end()) {

        SwapTypeEnum swapTypeEnum = it->second;

        switch (swapTypeEnum) {
            case PayerString:
                return VanillaSwap::Payer;
            case ReceiverString:
                return VanillaSwap::Receiver;
        }
    }else{
        //throw std::runtime_error(swapType + " is not a valid Swap Type");
        QL_FAIL(swapType + " is not a valid Swap Type");
    }
}

bool validBusinessDate(string businessDateString, Calendar cal){

    Date businessDate;

    try {
        businessDate = DateParser::parseFormatted(businessDateString, "%Y/%m/%d");
    }catch (Error e) {
        //throw std::runtime_error(businessDateString + " is not a valid Y/m/d Format. " + e.what());
        QL_FAIL(businessDateString + " is not a valid Y/m/d Format. " + e.what());
    }

    return  cal.isBusinessDay(businessDate);
}

std::map<std::string, BoolEnum> BoolString = map_list_of
        ("True", True)
        ("False", False);

bool boolToQL(string boolean){
    std::map<std::string, BoolEnum>::iterator it = BoolString.find(boolean);

    if(it != BoolString.end()) {

        BoolEnum boolEnum = it->second;

        switch (boolEnum) {
            case True:
                return true;
            case False:
                return false;
        }
    }else{
        //throw std::runtime_error(boolean + " is not a valid bool");
        QL_FAIL(boolean + " is not a valid bool");
    }
}

std::map<std::string, DateGenerationEnum> DateGenerationString = map_list_of
        ("Backward", BackwardString)
        ("Forward", ForwardString)
        ("Zero",ZeroString)
        ("ThirdWednesday", ThirdWednesdayString)
        ("Twentieth", TwentiethString)
        ("TwentiethIMM", TwentiethIMMString)
        ("OldCDS", OldCDSString)
        ("CDS", CDSString);

DateGeneration::Rule dateGenerationToQL(string dateGeneration){
    std::map<std::string, DateGenerationEnum>::iterator it = DateGenerationString.find(dateGeneration);

    if(it != DateGenerationString.end()) {

        DateGenerationEnum dateGenerationEnum = it->second;

        switch (dateGenerationEnum) {
            case BackwardString:
                return DateGeneration::Backward;
            case ForwardString:
                return DateGeneration::Forward;
            case ZeroString:
                return DateGeneration::Zero;
            case ThirdWednesdayString:
                return DateGeneration::ThirdWednesday;
            case TwentiethString:
                return DateGeneration::Twentieth;
            case TwentiethIMMString:
                return DateGeneration::TwentiethIMM;
            case OldCDSString:
                return DateGeneration::OldCDS;
            case CDSString:
                return DateGeneration::CDS;
        }
    }else{
        //throw std::runtime_error(dateGeneration + " is not a valid DateGeneration Rule");
        QL_FAIL(dateGeneration + " is not a valid DateGeneration Rule");
    }
}

std::map<std::string, WeekDayEnum> WeekDayString = map_list_of
        ("Sunday", SundayString)
        ("Monday", MondayString)
        ("Tuesday", TuesdayString)
        ("Wednesday", WednesdayString)
        ("Thursday", ThursdayString)
        ("Friday", FridayString)
        ("Saturday", SaturdayString);

Weekday  weekDayToQL(string weekDay){
    std::map<std::string, WeekDayEnum>::iterator it = WeekDayString.find(weekDay);

    if(it != WeekDayString.end()) {

        WeekDayEnum weekDayEnum = it->second;

        switch (weekDayEnum) {
            case SundayString:
                return Sunday;
            case MondayString:
                return Monday;
            case TuesdayString:
                return Tuesday;
            case WednesdayString:
                return Wednesday;
            case ThursdayString:
                return Thursday;
            case FridayString:
                return Friday;
            case SaturdayString:
                return Saturday;
        }
    }else{
        //throw std::runtime_error(dateGeneration + " is not a valid DateGeneration Rule");
        QL_FAIL(weekDay + " is not a valid WeekDay");
    }
}

std::map<std::string, MonthEnum> MonthString = map_list_of
        ("January", JanuaryString)
        ("February", FebruaryString)
        ("March", MarchString)
        ("April", AprilString)
        ("May", MayString)
        ("June", JuneString)
        ("July", JulyString)
        ("August", AugustString)
        ("September", SeptemberString)
        ("October", OctoberString)
        ("November", NovemberString)
        ("December", DecemberString);

Month monthToQL(string month){
    std::map<std::string, MonthEnum>::iterator it = MonthString.find(month);

    if(it != MonthString.end()) {

        MonthEnum monthEnum = it->second;

        switch (monthEnum) {
            case JanuaryString:
                return January;
            case FebruaryString:
                return February;
            case MarchString:
                return March;
            case AprilString:
                return April;
            case MayString:
                return May;
            case JuneString:
                return June;
            case JulyString:
                return July;
            case AugustString:
                return August;
            case SeptemberString:
                return September;
            case OctoberString:
                return October;
            case NovemberString:
                return November;
            case DecemberString:
                return December;
        }
    }else{
        //throw std::runtime_error(dateGeneration + " is not a valid DateGeneration Rule");
        QL_FAIL(month + " is not a valid Month");
    }
}

std::map<std::string, ZeroSpreadTypeEnum> ZeroSpreadTypeString = map_list_of
        ("Parallel", ParallelString)
        ("LinearInterpolated", LinearInterpolatedString);

ZeroSpreadTypeEnum ZeroSpreadTypeToQL(string zeroSpreadType){

    std::map<std::string, ZeroSpreadTypeEnum>::iterator it = ZeroSpreadTypeString.find(zeroSpreadType);

    if(it != ZeroSpreadTypeString.end()) {
        return it->second;
    }else{
        QL_FAIL(zeroSpreadType + " is not a valid Zero Spread");
    }
}

std::map<std::string, VolEnum> VolString = map_list_of
        ("BlackConstantVol", BlackConstantVolString)
        ("BlackVarianceSurface", BlackVarianceSurfaceString);

VolEnum volToQL(string comp){
    std::map<std::string, VolEnum>::iterator it = VolString.find(comp);

    if(it != VolString.end()) {
        return it->second;
    }else{
        QL_FAIL(comp + " is not a valid volatility type");
    }
}

std::map<std::string, OptionTypeEnum> OptionTypeString = map_list_of
        ("Put", PutString)
        ("Call", CallString);

Option::Type optionTypeToQL(string optionType){
    std::map<std::string, OptionTypeEnum>::iterator it = OptionTypeString.find(optionType);

    if(it != OptionTypeString.end()) {

        OptionTypeEnum optionTypeEnum = it->second;

        switch (optionTypeEnum) {
            case PutString:
                return Option::Put;
            case CallString:
                return Option::Call;
        }
    }else{
        //throw std::runtime_error(dateGeneration + " is not a valid DateGeneration Rule");
        QL_FAIL(optionType + " is not a valid Option Type");
    }
}

std::map<std::string, OptionStyleEnum> OptionStyleString = map_list_of
        ("European", EuropeanString);

OptionStyleEnum OptionStyleToQL(string OptionStyle){

    std::map<std::string, OptionStyleEnum>::iterator it = OptionStyleString.find(OptionStyle);

    if(it != OptionStyleString.end()) {
        return it->second;
    }else{
        QL_FAIL(OptionStyle + " is not a valid option style");
    }
}