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

    int n_bonds_x_request = 1;
    int n_requests = 1;
    int share_curve = 1;

    double total_quantra_npv = 0;
    double total_quantlib_npv = 0;

    if (argc > 1)
    {
        std::istringstream iss1(argv[1]);
        iss1 >> n_bonds_x_request;

        std::istringstream iss2(argv[2]);
        iss2 >> n_requests;

        std::istringstream iss3(argv[3]);
        iss3 >> share_curve;
    }

    int total_bonds = n_bonds_x_request * n_requests;

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    QuantraClient client("localhost:50051", false);

    auto term_structure = term_structure_example();
    auto bond = fixed_rate_bond_example();
    auto request = pricing_request_example();

    std::vector<std::shared_ptr<structs::TermStructure>> term_structures;
    std::vector<std::shared_ptr<structs::PriceFixedRateBond>> bonds;

    if (share_curve == 1)
        term_structures.push_back(term_structure);

    for (int i = 0; i < n_bonds_x_request; i++)
    {
        auto price_fixed_rate_bond = std::make_shared<structs::PriceFixedRateBond>();
        price_fixed_rate_bond->fixed_rate_bond = bond;
        strcpy(price_fixed_rate_bond->discounting_curve, "depos");

        bonds.push_back(price_fixed_rate_bond);

        if (share_curve == 0)
            term_structures.push_back(term_structure);
    }

    request->pricing->curves = term_structures;
    request->bonds = bonds;

    std::vector<std::shared_ptr<structs::PriceFixedRateBondRequest>> requests;

    for (int i = 0; i < n_requests; i++)
    {
        requests.push_back(request);
    }

    auto responses = client.PriceFixedRateBond(requests);

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    std::cout << "Quantra Time = " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() << "[ms]" << std::endl;
    int size1 = responses.size();
    int size2 = 0;
    for (auto it = responses.begin(); it != responses.end(); it++)
    {
        size2 = (*it)->size();
        for (auto it2 = (*it)->begin(); it2 != (*it)->end(); it2++)
        {
            total_quantra_npv += (*it2)->npv;
        }
    }

    std::cout << "Quantra NPV = " << total_quantra_npv << std::endl;

    // Quantlib execution

    begin = std::chrono::steady_clock::now();

    QuantLib::Calendar calendar = TARGET();

    Date settlementDate(18, September, 2008);

    Integer fixingDays = 3;
    Natural settlementDays = 3;

    Settings::instance().evaluationDate() = settlementDate;

    std::shared_ptr<PricingEngine> bondEngine;

    for (int i = 0; i < total_bonds; i++)
    {

        try
        {
            if ((share_curve == 1 && i == 0) || share_curve == 0)
            {
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

                QuantLib::DayCounter termStructureDayCounter =
                    ActualActual(ActualActual::ISDA);

                std::vector<std::shared_ptr<RateHelper>> bondInstruments;

                bondInstruments.push_back(zc3m);
                bondInstruments.push_back(zc6m);
                bondInstruments.push_back(zc1y);

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

                std::shared_ptr<PricingEngine> bondEngine2(
                    new DiscountingBondEngine(discountingTermStructure));

                //bondEngine = std::make_shared<PricingEngine>(new DiscountingBondEngine(discountingTermStructure));
                bondEngine = std::move(bondEngine2);
            }

            QuantLib::Schedule fixedBondSchedule(Date(15, May, 2007),
                                                 Date(15, May, 2017), Period(Semiannual),
                                                 UnitedStates(UnitedStates::GovernmentBond),
                                                 Unadjusted, Unadjusted, DateGeneration::Backward, false);

            Real faceAmount = 100;

            QuantLib::FixedRateBond fixedRateBond(
                settlementDays,
                faceAmount,
                fixedBondSchedule,
                std::vector<Rate>(1, 0.045),
                ActualActual(ActualActual::Bond),
                ModifiedFollowing,
                100.0, Date(15, May, 2007));

            fixedRateBond.setPricingEngine(bondEngine);

            total_quantlib_npv += fixedRateBond.NPV();
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

    end = std::chrono::steady_clock::now();

    std::cout << "Quantlib Time = " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() << "[ms]" << std::endl;
    std::cout << "Quantlib NPV = " << total_quantlib_npv << std::endl;

    return 0;
}
