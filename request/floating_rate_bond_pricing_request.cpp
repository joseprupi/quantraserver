#include "floating_rate_bond_pricing_request.h"

#include "bond_flow_builder.h"
#include "pricing_registry.h"

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<quantra::PriceFloatingRateBondResponse> FloatingRateBondPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const quantra::PriceFloatingRateBondRequest *request) const
{
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    FloatingRateBondParser bond_parser;
    PricerParser pricer_parser;

    Date as_of_date = Settings::instance().evaluationDate();

    PricingParser pricing_parser;
    auto pricing = pricing_parser.parse(request->pricing());
    Date settlement_date = DateToQL(pricing->settlement_date);

    std::map<std::string, std::shared_ptr<QuantLib::IborCouponPricer>> pricers_map;
    for (const auto& pricer_spec : reg.rates.couponPricers)
    {
        pricers_map.insert(std::make_pair(pricer_spec->id()->str(), pricer_parser.parse(pricer_spec)));
    }

    auto bond_pricings = request->bonds();
    std::vector<flatbuffers::Offset<quantra::FloatingRateBondResponse>> bonds_vector;

    for (auto it = bond_pricings->begin(); it != bond_pricings->end(); it++)
    {
        auto discounting_term_structure = reg.rates.curves.find(it->discounting_curve()->str());
        auto forwarding_term_structure = reg.rates.curves.find(it->forwarding_curve()->str());
        auto pricer = pricers_map.find(it->coupon_pricer()->str());

        if (discounting_term_structure == reg.rates.curves.end())
            QUANTRA_NOT_FOUND("Discounting curve not found: " + it->discounting_curve()->str());
        if (forwarding_term_structure == reg.rates.curves.end())
            QUANTRA_NOT_FOUND("Forwarding curve not found: " + it->forwarding_curve()->str());
        if (pricer == pricers_map.end())
            QUANTRA_NOT_FOUND("Coupon pricer not found: " + it->coupon_pricer()->str());

        // Link forwarding curve and parse bond with IndexRegistry
        bond_parser.linkForwardingTermStructure(forwarding_term_structure->second->currentLink());
        std::shared_ptr<QuantLib::FloatingRateBond> bond = bond_parser.parse(it->floating_rate_bond(), reg.rates.indices);

        std::vector<flatbuffers::Offset<quantra::FlowsWrapper>> flows_vector;
        std::shared_ptr<PricingEngine> bond_engine(new QuantLib::DiscountingBondEngine(*discounting_term_structure->second));
        bond->setPricingEngine(bond_engine);
        setCouponPricer(bond->cashflows(), pricer->second);

        auto npv = bond->NPV();

        if (reg.options.bondPricingFlows)
        {
            flows_vector = buildFloatingBondFlows(
                bond->cashflows(),
                discounting_term_structure->second->currentLink(),
                builder,
                as_of_date);
        }

        auto flows = builder->CreateVector(flows_vector);

        FloatingRateBondResponseBuilder response_builder(*builder);

        response_builder.add_flows(flows);
        response_builder.add_npv(bond->NPV());

        if (reg.options.bondPricingDetails)
        {
            YieldParser yield_parser;
            auto yield_struct = yield_parser.parse(it->yield());

            auto yield = bond->yield(yield_struct->day_counter, yield_struct->compounding,
                                     yield_struct->frequency);

            response_builder.add_clean_price(bond->cleanPrice());
            response_builder.add_dirty_price(bond->dirtyPrice());
            response_builder.add_accrued_amount(bond->accruedAmount());
            response_builder.add_yield(yield);
            response_builder.add_accrued_days(BondFunctions::accruedDays(*bond));

            InterestRate interest_rate(yield, DayCounterToQL(it->yield()->day_counter()),
                                       CompoundingToQL(it->yield()->compounding()),
                                       FrequencyToQL(it->yield()->frequency()));

            response_builder.add_modified_duration(BondFunctions::duration(*bond, interest_rate,
                                                                           Duration::Modified, settlement_date));
            response_builder.add_macaulay_duration(BondFunctions::duration(*bond, interest_rate,
                                                                           Duration::Macaulay, settlement_date));
            response_builder.add_convexity(BondFunctions::convexity(*bond, interest_rate, settlement_date));
            response_builder.add_bps(BondFunctions::bps(*bond, *discounting_term_structure->second->currentLink(), settlement_date));
        }

        auto bond_response = response_builder.Finish();
        bonds_vector.push_back(bond_response);
    }

    auto bonds = builder->CreateVector(bonds_vector);
    PriceFloatingRateBondResponseBuilder response_builder(*builder);
    response_builder.add_bonds(bonds);

    return response_builder.Finish();
}
