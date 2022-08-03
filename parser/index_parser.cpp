#include "index_parser.h"

std::shared_ptr<QuantLib::IborIndex> IndexParser::parse(const quantra::Index *index)
{
    if (index == NULL)
        QUANTRA_ERROR("Index not found");

    QuantLib::IndexManager::instance().clearHistories();

    std::shared_ptr<QuantLib::IborIndex> ibor_index(
        new QuantLib::IborIndex(
            "index",
            index->period_number() * TimeUnitToQL(index->period_time_unit()),
            index->settlement_days(),
            QuantLib::EURCurrency(),
            CalendarToQL(index->calendar()),
            ConventionToQL(index->business_day_convention()),
            index->end_of_month(),
            DayCounterToQL(index->day_counter()),
            this->term_structure));

    auto fixings = index->fixings();

    for (int i = 0; i < fixings->size(); i++)
    {
        auto fixing = fixings->Get(i);
        ibor_index->addFixing(DateToQL(fixing->date()->str()), fixing->rate());
    }

    return ibor_index;
}

void IndexParser::link_term_structure(std::shared_ptr<YieldTermStructure> term_structure)
{
    this->term_structure.linkTo(term_structure);
}

// #include "index_parser.h"

// std::shared_ptr<QuantLib::IborIndex> IndexParser::parse(const quantra::Index *index)
// {
//     if (index == NULL)
//         QUANTRA_ERROR("Index not found");

//     QuantLib::IndexManager::instance().clearHistories();

//     /* Test */

//     Date settlementDate(18, September, 2008);

//     DayCounter termStructureDayCounter =
//         ActualActual(ActualActual::ISDA);

//     Integer fixingDays = 3;
//     Calendar calendar = TARGET();

//     Rate d1wQuote = 0.043375;
//     Rate d1mQuote = 0.031875;
//     Rate d3mQuote = 0.0320375;
//     Rate d6mQuote = 0.03385;
//     Rate d9mQuote = 0.0338125;
//     Rate d1yQuote = 0.0335125;
//     // swaps
//     Rate s2yQuote = 0.0295;
//     Rate s3yQuote = 0.0323;
//     Rate s5yQuote = 0.0359;
//     Rate s10yQuote = 0.0412;
//     Rate s15yQuote = 0.0433;

//     ext::shared_ptr<Quote> d1wRate(new SimpleQuote(d1wQuote));
//     ext::shared_ptr<Quote> d1mRate(new SimpleQuote(d1mQuote));
//     ext::shared_ptr<Quote> d3mRate(new SimpleQuote(d3mQuote));
//     ext::shared_ptr<Quote> d6mRate(new SimpleQuote(d6mQuote));
//     ext::shared_ptr<Quote> d9mRate(new SimpleQuote(d9mQuote));
//     ext::shared_ptr<Quote> d1yRate(new SimpleQuote(d1yQuote));
//     // swaps
//     ext::shared_ptr<Quote> s2yRate(new SimpleQuote(s2yQuote));
//     ext::shared_ptr<Quote> s3yRate(new SimpleQuote(s3yQuote));
//     ext::shared_ptr<Quote> s5yRate(new SimpleQuote(s5yQuote));
//     ext::shared_ptr<Quote> s10yRate(new SimpleQuote(s10yQuote));
//     ext::shared_ptr<Quote> s15yRate(new SimpleQuote(s15yQuote));

//     // deposits
//     DayCounter depositDayCounter = Actual360();

//     ext::shared_ptr<RateHelper> d1w(new DepositRateHelper(
//         Handle<Quote>(d1wRate),
//         1 * Weeks, fixingDays,
//         calendar, ModifiedFollowing,
//         true, depositDayCounter));
//     ext::shared_ptr<RateHelper> d1m(new DepositRateHelper(
//         Handle<Quote>(d1mRate),
//         1 * Months, fixingDays,
//         calendar, ModifiedFollowing,
//         true, depositDayCounter));
//     ext::shared_ptr<RateHelper> d3m(new DepositRateHelper(
//         Handle<Quote>(d3mRate),
//         3 * Months, fixingDays,
//         calendar, ModifiedFollowing,
//         true, depositDayCounter));
//     ext::shared_ptr<RateHelper> d6m(new DepositRateHelper(
//         Handle<Quote>(d6mRate),
//         6 * Months, fixingDays,
//         calendar, ModifiedFollowing,
//         true, depositDayCounter));
//     ext::shared_ptr<RateHelper> d9m(new DepositRateHelper(
//         Handle<Quote>(d9mRate),
//         9 * Months, fixingDays,
//         calendar, ModifiedFollowing,
//         true, depositDayCounter));
//     ext::shared_ptr<RateHelper> d1y(new DepositRateHelper(
//         Handle<Quote>(d1yRate),
//         1 * Years, fixingDays,
//         calendar, ModifiedFollowing,
//         true, depositDayCounter));

//     // setup swaps
//     Frequency swFixedLegFrequency = Annual;
//     BusinessDayConvention swFixedLegConvention = Unadjusted;
//     DayCounter swFixedLegDayCounter = Thirty360(Thirty360::European);
//     ext::shared_ptr<IborIndex> swFloatingLegIndex(new Euribor6M);

//     const Period forwardStart(1 * Days);

//     ext::shared_ptr<RateHelper> s2y(new SwapRateHelper(
//         Handle<Quote>(s2yRate), 2 * Years,
//         calendar, swFixedLegFrequency,
//         swFixedLegConvention, swFixedLegDayCounter,
//         swFloatingLegIndex, Handle<Quote>(), forwardStart));
//     ext::shared_ptr<RateHelper> s3y(new SwapRateHelper(
//         Handle<Quote>(s3yRate), 3 * Years,
//         calendar, swFixedLegFrequency,
//         swFixedLegConvention, swFixedLegDayCounter,
//         swFloatingLegIndex, Handle<Quote>(), forwardStart));
//     ext::shared_ptr<RateHelper> s5y(new SwapRateHelper(
//         Handle<Quote>(s5yRate), 5 * Years,
//         calendar, swFixedLegFrequency,
//         swFixedLegConvention, swFixedLegDayCounter,
//         swFloatingLegIndex, Handle<Quote>(), forwardStart));
//     ext::shared_ptr<RateHelper> s10y(new SwapRateHelper(
//         Handle<Quote>(s10yRate), 10 * Years,
//         calendar, swFixedLegFrequency,
//         swFixedLegConvention, swFixedLegDayCounter,
//         swFloatingLegIndex, Handle<Quote>(), forwardStart));
//     ext::shared_ptr<RateHelper> s15y(new SwapRateHelper(
//         Handle<Quote>(s15yRate), 15 * Years,
//         calendar, swFixedLegFrequency,
//         swFixedLegConvention, swFixedLegDayCounter,
//         swFloatingLegIndex, Handle<Quote>(), forwardStart));

//     // A depo-swap curve
//     std::vector<ext::shared_ptr<RateHelper>> depoSwapInstruments;
//     depoSwapInstruments.push_back(d1w);
//     depoSwapInstruments.push_back(d1m);
//     depoSwapInstruments.push_back(d3m);
//     depoSwapInstruments.push_back(d6m);
//     depoSwapInstruments.push_back(d9m);
//     depoSwapInstruments.push_back(d1y);
//     depoSwapInstruments.push_back(s2y);
//     depoSwapInstruments.push_back(s3y);
//     depoSwapInstruments.push_back(s5y);
//     depoSwapInstruments.push_back(s10y);
//     depoSwapInstruments.push_back(s15y);
//     ext::shared_ptr<YieldTermStructure> depoSwapTermStructure(
//         new PiecewiseYieldCurve<Discount, LogLinear>(
//             settlementDate, depoSwapInstruments,
//             termStructureDayCounter));

//     RelinkableHandle<YieldTermStructure> liborTermStructure;
//     const ext::shared_ptr<IborIndex> libor3m(
//         new USDLibor(Period(3, Months), liborTermStructure));
//     libor3m->addFixing(Date(17, July, 2008), 0.0278625);

//     liborTermStructure.linkTo(depoSwapTermStructure);

//     /* Test */

//     std::shared_ptr<QuantLib::IborIndex> ibor_index(
//         new QuantLib::IborIndex(
//             "index",
//             index->period_number() * TimeUnitToQL(index->period_time_unit()),
//             index->settlement_days(),
//             QuantLib::EURCurrency(),
//             CalendarToQL(index->calendar()),
//             ConventionToQL(index->business_day_convention()),
//             index->end_of_month(),
//             DayCounterToQL(index->day_counter()),
//             // this->term_structure));
//             liborTermStructure));

//     auto fixings = index->fixings();

//     for (int i = 0; i < fixings->size(); i++)
//     {
//         auto fixing = fixings->Get(i);
//         ibor_index->addFixing(DateToQL(fixing->date()->str()), fixing->rate());
//     }

//     return ibor_index;
// }

// void IndexParser::link_term_structure(std::shared_ptr<YieldTermStructure> term_structure)
// {
//     this->term_structure.linkTo(term_structure);
// }
