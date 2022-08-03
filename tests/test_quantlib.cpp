#include <vector>

#include "quantra_client.h"
#include "request_data.h"

#include <ql/instruments/bonds/zerocouponbond.hpp>
#include <ql/instruments/bonds/floatingratebond.hpp>
#include <ql/pricingengines/bond/discountingbondengine.hpp>
#include <ql/cashflows/couponpricer.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/bondhelpers.hpp>
#include <ql/termstructures/volatility/optionlet/constantoptionletvol.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/indexes/ibor/usdlibor.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/calendars/unitedstates.hpp>
#include <ql/time/daycounters/actualactual.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/thirty360.hpp>

using namespace QuantLib;

int main(int argc, char **argv)
{

    int total_bonds = 1;

    double total_quantra_npv = 0;
    double total_quantlib_npv = 0;

    if (argc > 1)
    {
        std::istringstream iss1(argv[1]);
        iss1 >> total_bonds;
    }

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    for (int i = 0; i < total_bonds; i++)
    {

        try
        {

            QuantLib::Calendar calendar = TARGET();

            Date settlementDate(18, September, 2008);
            settlementDate = calendar.adjust(settlementDate);

            Integer fixingDays = 3;
            Natural settlementDays = 3;

            Date todaysDate = calendar.advance(settlementDate, -fixingDays, Days);
            Settings::instance().evaluationDate() = todaysDate;

            Rate zc3mQuote = 0.0096;
            Rate zc6mQuote = 0.0145;
            Rate zc1yQuote = 0.0194;

            std::shared_ptr<Quote> zc3mRate(new SimpleQuote(zc3mQuote));
            std::shared_ptr<Quote> zc6mRate(new SimpleQuote(zc6mQuote));
            std::shared_ptr<Quote> zc1yRate(new SimpleQuote(zc1yQuote));

            QuantLib::DayCounter zcBondsDayCounter = Actual365Fixed();

            std::shared_ptr<RateHelper> zc3m(new DepositRateHelper(
                Handle<Quote>(zc3mRate),
                3 * Months, fixingDays,
                calendar, ModifiedFollowing,
                true, zcBondsDayCounter));
            std::shared_ptr<RateHelper> zc6m(new DepositRateHelper(
                Handle<Quote>(zc6mRate),
                6 * Months, fixingDays,
                calendar, ModifiedFollowing,
                true, zcBondsDayCounter));
            std::shared_ptr<RateHelper> zc1y(new DepositRateHelper(
                Handle<Quote>(zc1yRate),
                1 * Years, fixingDays,
                calendar, ModifiedFollowing,
                true, zcBondsDayCounter));

            Real redemption = 100.0;

            const Size numberOfBonds = 5;

            Date issueDates[] = {
                Date(15, March, 2005),
                Date(15, June, 2005),
                Date(30, June, 2006),
                Date(15, November, 2002),
                Date(15, May, 1987)};

            Date maturities[] = {
                Date(31, August, 2010),
                Date(31, August, 2011),
                Date(31, August, 2013),
                Date(15, August, 2018),
                Date(15, May, 2038)};

            Real couponRates[] = {
                0.02375,
                0.04625,
                0.03125,
                0.04000,
                0.04500};

            Real marketQuotes[] = {
                100.390625,
                106.21875,
                100.59375,
                101.6875,
                102.140625};

            std::vector<std::shared_ptr<SimpleQuote>> quote;
            for (double marketQuote : marketQuotes)
            {
                std::shared_ptr<SimpleQuote> cp(new SimpleQuote(marketQuote));
                quote.push_back(cp);
            }

            RelinkableHandle<Quote> quoteHandle[numberOfBonds];
            for (Size i = 0; i < numberOfBonds; i++)
            {
                quoteHandle[i].linkTo(quote[i]);
            }

            std::vector<std::shared_ptr<QuantLib::BondHelper>> bondsHelpers;

            for (Size i = 0; i < numberOfBonds; i++)
            {

                QuantLib::Schedule schedule(issueDates[i], maturities[i], Period(Semiannual), UnitedStates(UnitedStates::GovernmentBond),
                                            Unadjusted, Unadjusted, DateGeneration::Backward, false);

                std::shared_ptr<FixedRateBondHelper> bondHelper(new FixedRateBondHelper(
                    quoteHandle[i],
                    settlementDays,
                    100.0,
                    schedule,
                    std::vector<Rate>(1, couponRates[i]),
                    ActualActual(ActualActual::Bond),
                    Unadjusted,
                    redemption,
                    issueDates[i]));

                bondsHelpers.push_back(bondHelper);
            }

            /*********************
         **  CURVE BUILDING **
         *********************/

            // Any DayCounter would be fine.
            // ActualActual::ISDA ensures that 30 years is 30.0
            QuantLib::DayCounter termStructureDayCounter =
                ActualActual(ActualActual::ISDA);

            // A depo-bond curve
            std::vector<std::shared_ptr<RateHelper>> bondInstruments;

            // Adding the ZC bonds to the curve for the short end
            bondInstruments.push_back(zc3m);
            bondInstruments.push_back(zc6m);
            bondInstruments.push_back(zc1y);

            // Adding the Fixed rate bonds to the curve for the long end
            for (Size i = 0; i < numberOfBonds; i++)
            {
                bondInstruments.push_back(bondsHelpers[i]);
            }

            std::shared_ptr<YieldTermStructure> bondDiscountingTermStructure(
                new PiecewiseYieldCurve<Discount, LogLinear>(
                    settlementDate, bondInstruments,
                    termStructureDayCounter));

            RelinkableHandle<YieldTermStructure> discountingTermStructure;
            discountingTermStructure.linkTo(bondDiscountingTermStructure);

            // Common data
            Real faceAmount = 100;

            // Pricing engine
            std::shared_ptr<PricingEngine> bondEngine(
                new DiscountingBondEngine(discountingTermStructure));

            // Fixed 4.5% US Treasury Note
            QuantLib::Schedule fixedBondSchedule(Date(15, May, 2007),
                                                 Date(15, May, 2017), Period(Semiannual),
                                                 UnitedStates(UnitedStates::GovernmentBond),
                                                 Unadjusted, Unadjusted, DateGeneration::Backward, false);

            QuantLib::FixedRateBond fixedRateBond(
                settlementDays,
                faceAmount,
                fixedBondSchedule,
                std::vector<Rate>(1, 0.045),
                ActualActual(ActualActual::Bond),
                ModifiedFollowing,
                100.0, Date(15, May, 2007));

            fixedRateBond.setPricingEngine(bondEngine);

            auto result = fixedRateBond.NPV() + 1;
        }
        catch (std::exception &e)
        {
            std::cerr << e.what() << std::endl;
            return 1;
        }
        catch (...)
        {
            std::cerr << "unknown error" << std::endl;
            return 1;
        }
    }

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    std::cout << "Quantlib Time = " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() << "[ms]" << std::endl;

    return 0;
}
