#include "fixed_rate_bond_pricing_request.h"

#include "bond_flow_builder.h"
#include "pricing_registry.h"

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<quantra::PriceFixedRateBondResponse> FixedRateBondPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const quantra::PriceFixedRateBondRequest *request) const
{
    // Build registry (handles curves with dependency ordering via CurveBootstrapper)
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    FixedRateBondParser bond_parser;

    Date as_of_date = Settings::instance().evaluationDate();

    // Get settlement date from pricing
    PricingParser pricing_parser;
    auto pricing = pricing_parser.parse(request->pricing());
    Date settlement_date = DateToQL(pricing->settlement_date);

    auto bond_pricings = request->bonds();
    std::vector<flatbuffers::Offset<quantra::FixedRateBondResponse>> bonds_vector;

    for (auto it = bond_pricings->begin(); it != bond_pricings->end(); it++)
    {
        auto term_structure = reg.curves.find(it->discounting_curve()->str());

        if (term_structure == reg.curves.end())
        {
            QUANTRA_ERROR("Discounting curve not found: " + it->discounting_curve()->str());
        }

        std::shared_ptr<QuantLib::FixedRateBond> bond = bond_parser.parse(it->fixed_rate_bond());
        std::vector<flatbuffers::Offset<quantra::FlowsWrapper>> flows_vector;
        std::shared_ptr<PricingEngine> bond_engine(new QuantLib::DiscountingBondEngine(*term_structure->second));
        bond->setPricingEngine(bond_engine);

        if (reg.bondPricingFlows)
        {
            flows_vector = buildFixedBondFlows(
                bond->cashflows(),
                term_structure->second->currentLink(),
                builder,
                as_of_date);
        }

        const double npv = bond->NPV();
        bool hasDetails = reg.bondPricingDetails;
        double cleanPrice = 0.0;
        double dirtyPrice = 0.0;
        double accruedAmount = 0.0;
        double yield = 0.0;
        uint32_t accruedDays = 0;
        double modifiedDuration = 0.0;
        double macaulayDuration = 0.0;
        double convexity = 0.0;
        double bps = 0.0;

        if (hasDetails)
        {
            YieldParser yield_parser;
            auto yield_struct = yield_parser.parse(it->yield());

            yield = bond->yield(
                yield_struct->day_counter,
                yield_struct->compounding,
                yield_struct->frequency);

            cleanPrice = bond->cleanPrice();
            dirtyPrice = bond->dirtyPrice();
            accruedAmount = bond->accruedAmount();
            accruedDays = BondFunctions::accruedDays(*bond);

            InterestRate interest_rate(
                yield,
                DayCounterToQL(it->yield()->day_counter()),
                CompoundingToQL(it->yield()->compounding()),
                FrequencyToQL(it->yield()->frequency()));

            modifiedDuration = BondFunctions::duration(*bond, interest_rate, Duration::Modified, settlement_date);
            macaulayDuration = BondFunctions::duration(*bond, interest_rate, Duration::Macaulay, settlement_date);
            convexity = BondFunctions::convexity(*bond, interest_rate, settlement_date);
            bps = BondFunctions::bps(*bond, *term_structure->second->currentLink(), settlement_date);
        }

        auto flows = builder->CreateVector(flows_vector);

        FixedRateBondResponseBuilder response_builder(*builder);

        response_builder.add_flows(flows);
        response_builder.add_npv(npv);

        if (hasDetails)
        {
            response_builder.add_clean_price(cleanPrice);
            response_builder.add_dirty_price(dirtyPrice);
            response_builder.add_accrued_amount(accruedAmount);
            response_builder.add_yield(yield);
            response_builder.add_accrued_days(accruedDays);
            response_builder.add_modified_duration(modifiedDuration);
            response_builder.add_macaulay_duration(macaulayDuration);
            response_builder.add_convexity(convexity);
            response_builder.add_bps(bps);
        }

        auto bond_response = response_builder.Finish();
        bonds_vector.push_back(bond_response);
    }

    auto bonds = builder->CreateVector(bonds_vector);
    PriceFixedRateBondResponseBuilder response_builder(*builder);
    response_builder.add_bonds(bonds);

    return response_builder.Finish();
}
