// Equity option payoffs that read one Black vol at the strike (cash-or-nothing
// and asset-or-nothing digitals, barriers) against the shape of the vol they
// reference: refused with a 400 on a vol that depends on strike (a strike x
// expiry grid, SVI slices, eSSVI slices), priced as before on a Constant or
// an AtmMatrix2D (term) vol, bit-identical to QuantLib's engine on the same
// process. A plain vanilla prices on every shape.
#include "equity_option_evaluator.h"
#include "error.h"
#include "model_domain.h"
#include "smile_slice_vol_surface.h"
#include "ssvi_vol_surface.h"

#include <ql/exercise.hpp>
#include <ql/instruments/barrieroption.hpp>
#include <ql/instruments/payoffs.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/pricingengines/barrier/analyticbarrierengine.hpp>
#include <ql/pricingengines/vanilla/analyticdigitalamericanengine.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancecurve.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace quantra { namespace testing {

namespace {

using QuantLib::Date;
using QuantLib::Handle;

struct ShapeMarket {
    Date asOf{21, QuantLib::October, 2026};
    Date expiry{21, QuantLib::July, 2027};
    QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    QuantLib::Calendar cal = QuantLib::NullCalendar();
    double spot = 100.0;
    double r = 0.04, q = 0.013;
    PricingRegistry reg;
    PricingContext ctx;
    std::vector<Date> expiries{asOf + 91, asOf + 182, asOf + 365, asOf + 730};
    std::vector<double> strikes{70.0, 85.0, 100.0, 115.0, 130.0};

    ShapeMarket() {
        QuantLib::Settings::instance().evaluationDate() = asOf;
        ctx.asOf = asOf;
        reg.rates.curves["disc"] =
            std::make_shared<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>(
                std::make_shared<QuantLib::FlatForward>(asOf, r, dc));
        EquityUnderlyingRuntime u;
        u.currency = "USD";
        u.spot = Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(spot));
        u.dividend = Handle<QuantLib::YieldTermStructure>(
            std::make_shared<QuantLib::FlatForward>(asOf, q, dc));
        reg.equity.equityUnderlyings["EQ"] = u;
        reg.volatility.modelDomains["bs"] = ModelDomain{"bs", EquityVanillaModelDomain{}};

        add("constant", quantra::enums::VolSurfaceShape_Constant,
            std::make_shared<QuantLib::BlackConstantVol>(asOf, cal, 0.20, dc));
        add("term", quantra::enums::VolSurfaceShape_AtmMatrix2D,
            std::make_shared<QuantLib::BlackVarianceCurve>(
                asOf, expiries, std::vector<double>{0.22, 0.21, 0.20, 0.19}, dc, true));

        // A skewed grid read CubicStrikeLinearTime.
        std::vector<std::vector<double>> rows;
        for (std::size_t i = 0; i < expiries.size(); ++i) {
            std::vector<double> row;
            for (double K : strikes)
                row.push_back(0.20 - 0.10 * std::log(K / spot));
            rows.push_back(row);
        }
        add("grid", quantra::enums::VolSurfaceShape_SmileCube3D,
            makeCubicStrikeLinearTimeSurface(
                asOf, cal, QuantLib::Unadjusted, dc, expiries, strikes, rows));

        std::vector<SviSliceParams> svi;
        for (const Date& d : expiries) {
            const double T = dc.yearFraction(asOf, d);
            SviSliceParams p;
            p.forward = spot * std::exp((r - q) * T);
            p.a = 0.03 * T;
            p.b = 0.10 * std::sqrt(T);
            p.sigma = 0.10;
            p.rho = -0.7;
            p.m = 0.0;
            svi.push_back(p);
        }
        add("svi", quantra::enums::VolSurfaceShape_SviSlices,
            makeSviSurface(asOf, cal, QuantLib::Unadjusted, dc, expiries, svi));

        std::vector<SsviSliceParams> ssvi{
            {0.010000, -0.70, 0.10},
            {0.018050, -0.68, 0.14},
            {0.034225, -0.65, 0.20},
            {0.064800, -0.60, 0.28}};
        std::vector<double> forwards;
        for (const Date& d : expiries)
            forwards.push_back(spot * std::exp((r - q) * dc.yearFraction(asOf, d)));
        add("ssvi", quantra::enums::VolSurfaceShape_SsviSlices,
            makeSsviSurface(asOf, cal, QuantLib::Unadjusted, dc, expiries, ssvi, forwards, spot));
    }

    void add(
        const std::string& id,
        quantra::enums::VolSurfaceShape shape,
        const std::shared_ptr<QuantLib::BlackVolTermStructure>& vol) {
        BlackVolEntry e;
        e.handle = Handle<QuantLib::BlackVolTermStructure>(vol);
        e.shapeFb = shape;
        e.referenceDate = asOf;
        e.calendar = cal;
        e.dayCounter = dc;
        reg.volatility.blackVols[id] = e;
    }

    EquityOptionTrade trade(const std::string& volId, EquityPayoffKind kind) const {
        EquityOptionTrade t;
        t.tradeId = "T";
        t.optionType = QuantLib::Option::Call;
        t.strike = 105.0;
        t.payoffKind = kind;
        t.cash = 1.0;
        t.exercise = std::make_shared<QuantLib::EuropeanExercise>(expiry);
        t.discountingCurveId = "disc";
        t.underlyingId = "EQ";
        t.volatilityId = volId;
        t.modelId = "bs";
        return t;
    }

    EquityOptionTrade barrier(const std::string& volId) const {
        EquityOptionTrade t = trade(volId, EquityPayoffKind::PlainVanilla);
        t.hasBarrier = true;
        t.barrierType = QuantLib::Barrier::DownOut;
        t.barrierLevel = 80.0;
        t.rebate = 0.0;
        return t;
    }

    double price(const EquityOptionTrade& t) const {
        EquityOptionInputs in;
        in.trades.push_back(t);
        return EquityOptionEvaluator().evaluate(in, reg, ctx).trades.at(0).npv;
    }

    std::string refusal(const EquityOptionTrade& t) const {
        EquityOptionInputs in;
        in.trades.push_back(t);
        try {
            EquityOptionEvaluator().evaluate(in, reg, ctx);
        } catch (const QuantraInvalidArgument& e) {
            return e.what();
        }
        ADD_FAILURE() << "no error for vol " << t.volatilityId;
        return "";
    }

    std::shared_ptr<QuantLib::BlackScholesMertonProcess> process(const std::string& volId) const {
        const auto& u = reg.equity.equityUnderlyings.at("EQ");
        return std::make_shared<QuantLib::BlackScholesMertonProcess>(
            u.spot, u.dividend,
            Handle<QuantLib::YieldTermStructure>(reg.rates.curves.at("disc")->currentLink()),
            reg.volatility.blackVols.at(volId).handle);
    }
};

void expectNamed(const std::string& msg, const std::string& volId, const std::string& product) {
    EXPECT_NE(msg.find("volatility '" + volId + "'"), std::string::npos) << msg;
    EXPECT_NE(msg.find(product), std::string::npos) << msg;
    EXPECT_NE(msg.find("single vol read at the strike"), std::string::npos) << msg;
    EXPECT_NE(msg.find("Constant or an AtmMatrix2D"), std::string::npos) << msg;
}

} // namespace

TEST(EquityOptionVolShape, DigitalsAndBarriersRefusedOnStrikeDependentVols) {
    ShapeMarket m;
    for (const std::string id : {"grid", "svi", "ssvi"}) {
        expectNamed(
            m.refusal(m.trade(id, EquityPayoffKind::CashOrNothing)), id,
            "a cash-or-nothing payoff");
        expectNamed(
            m.refusal(m.trade(id, EquityPayoffKind::AssetOrNothing)), id,
            "an asset-or-nothing payoff");
        expectNamed(m.refusal(m.barrier(id)), id, "a barrier option");
        // American exercise of a digital reads one vol too.
        auto american = m.trade(id, EquityPayoffKind::CashOrNothing);
        american.exercise = std::make_shared<QuantLib::AmericanExercise>(m.asOf, m.expiry);
        expectNamed(m.refusal(american), id, "a cash-or-nothing payoff");
    }
    EXPECT_NE(
        m.refusal(m.trade("grid", EquityPayoffKind::CashOrNothing)).find("SmileCube3D"),
        std::string::npos);
    EXPECT_NE(
        m.refusal(m.trade("svi", EquityPayoffKind::CashOrNothing)).find("SVI slices"),
        std::string::npos);
    EXPECT_NE(
        m.refusal(m.trade("ssvi", EquityPayoffKind::CashOrNothing)).find("eSSVI slices"),
        std::string::npos);
}

TEST(EquityOptionVolShape, PlainVanillaPricesOnEveryShape) {
    ShapeMarket m;
    for (const std::string id : {"constant", "term", "grid", "svi", "ssvi"}) {
        const double npv = m.price(m.trade(id, EquityPayoffKind::PlainVanilla));
        EXPECT_TRUE(std::isfinite(npv) && npv > 0.0) << id;
    }
}

TEST(EquityOptionVolShape, DigitalsAndBarriersOnFlatVolsMatchQuantLib) {
    ShapeMarket m;
    const auto exercise = std::make_shared<QuantLib::EuropeanExercise>(m.expiry);
    for (const std::string id : {"constant", "term"}) {
        const auto process = m.process(id);
        {
            QuantLib::VanillaOption o(
                std::make_shared<QuantLib::CashOrNothingPayoff>(QuantLib::Option::Call, 105.0, 1.0),
                exercise);
            o.setPricingEngine(std::make_shared<QuantLib::AnalyticEuropeanEngine>(process));
            EXPECT_EQ(m.price(m.trade(id, EquityPayoffKind::CashOrNothing)), o.NPV()) << id;
        }
        {
            QuantLib::VanillaOption o(
                std::make_shared<QuantLib::AssetOrNothingPayoff>(QuantLib::Option::Call, 105.0),
                exercise);
            o.setPricingEngine(std::make_shared<QuantLib::AnalyticEuropeanEngine>(process));
            EXPECT_EQ(m.price(m.trade(id, EquityPayoffKind::AssetOrNothing)), o.NPV()) << id;
        }
        {
            QuantLib::VanillaOption o(
                std::make_shared<QuantLib::CashOrNothingPayoff>(QuantLib::Option::Call, 105.0, 1.0),
                std::make_shared<QuantLib::AmericanExercise>(m.asOf, m.expiry));
            o.setPricingEngine(std::make_shared<QuantLib::AnalyticDigitalAmericanEngine>(process));
            auto t = m.trade(id, EquityPayoffKind::CashOrNothing);
            t.exercise = std::make_shared<QuantLib::AmericanExercise>(m.asOf, m.expiry);
            EXPECT_EQ(m.price(t), o.NPV()) << id;
        }
        {
            QuantLib::BarrierOption o(
                QuantLib::Barrier::DownOut, 80.0, 0.0,
                std::make_shared<QuantLib::PlainVanillaPayoff>(QuantLib::Option::Call, 105.0),
                exercise);
            o.setPricingEngine(std::make_shared<QuantLib::AnalyticBarrierEngine>(process));
            EXPECT_EQ(m.price(m.barrier(id)), o.NPV()) << id;
        }
    }
}

}} // namespace quantra::testing
