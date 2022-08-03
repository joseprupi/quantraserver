#include "floating_rate_bond_pricing_request.h"

flatbuffers::Offset<quantra::PriceFloatingRateBondResponse> FloatingRateBondPricingRequest::request(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const quantra::PriceFloatingRateBondRequest *request) const
{

    PricingParser pricing_parser = PricingParser();
    FloatingRateBondParser bond_parser = FloatingRateBondParser();
    TermStructureParser term_structure_parser = TermStructureParser();
    PricerParser pricer_parser = PricerParser();

    auto pricing = pricing_parser.parse(request->pricing());

    Date as_of_date = DateToQL(pricing->as_of_date);
    Settings::instance().evaluationDate() = as_of_date;

    Date settlement_date = DateToQL(pricing->settlement_date);

    auto curves = pricing->curves;
    auto pricers = pricing->coupon_pricers;

    std::map<std::string, std::shared_ptr<RelinkableHandle<YieldTermStructure>>> term_structures;

    for (auto it = curves->begin(); it != curves->end(); it++)
    {

        auto yield_term_structure = std::make_shared<RelinkableHandle<YieldTermStructure>>();
        std::shared_ptr<YieldTermStructure> term_structure = term_structure_parser.parse(*it);
        yield_term_structure->linkTo(term_structure);
        term_structures.insert(std::make_pair(it->id()->str(), yield_term_structure));
    }

    std::map<std::string, std::shared_ptr<QuantLib::IborCouponPricer>> pricers_map;

    for (auto it = pricers->begin(); it != pricers->end(); it++)
    {
        pricers_map.insert(std::make_pair(it->id()->str(), pricer_parser.parse(*it)));
    }

    auto bond_pricings = request->bonds();

    std::vector<flatbuffers::Offset<quantra::FloatingRateBondResponse>> bonds_vector;

    for (auto it = bond_pricings->begin(); it != bond_pricings->end(); it++)
    {

        auto discounting_term_structure = term_structures.find(it->discounting_curve()->str());
        auto forecasting_term_structure = term_structures.find(it->forecasting_curve()->str());
        auto pricer = pricers_map.find(it->coupon_pricer()->str());

        if (discounting_term_structure == term_structures.end() || forecasting_term_structure == term_structures.end() || pricer == pricers_map.end())
        {
            PriceFloatingRateBondResponseBuilder response_builder(*builder);
            return response_builder.Finish();
        }
        else
        {
            std::shared_ptr<QuantLib::FloatingRateBond> bond = bond_parser.parse(it->floating_rate_bond());
            bond_parser.index_parser.link_term_structure(forecasting_term_structure->second->currentLink());
            std::vector<flatbuffers::Offset<quantra::FlowsWrapper>> flows_vector;
            std::shared_ptr<PricingEngine> bond_engine(new QuantLib::DiscountingBondEngine(*discounting_term_structure->second));
            bond->setPricingEngine(bond_engine);
            setCouponPricer(bond->cashflows(), pricer->second);

            auto npv = bond->NPV();

            if (pricing->bond_pricing_flows)
            {
                const Leg &cashflows = bond->cashflows();

                for (auto it = cashflows.begin(); it != cashflows.end(); ++it)
                {
                    auto coupon = std::dynamic_pointer_cast<FloatingRateCoupon>(*it);
                    if (coupon)
                    {
                        if (!coupon->hasOccurred(Settings::instance().evaluationDate()))
                        {
                            std::ostringstream os_start_date;
                            os_start_date << QuantLib::io::iso_date(coupon->accrualStartDate());
                            auto accrual_start_date = builder->CreateString(os_start_date.str());

                            std::ostringstream os_end_date;
                            os_end_date << QuantLib::io::iso_date(coupon->accrualEndDate());
                            auto accrual_end_date = builder->CreateString(os_end_date.str());

                            auto flow_interest_builder = FlowInterestBuilder(*builder);
                            flow_interest_builder.add_amount(coupon->amount());
                            flow_interest_builder.add_fixing_date(coupon->indexFixing());
                            flow_interest_builder.add_accrual_start_date(accrual_start_date);
                            flow_interest_builder.add_accrual_end_date(accrual_end_date);
                            flow_interest_builder.add_rate(coupon->rate());
                            flow_interest_builder.add_discount(discounting_term_structure->second->currentLink()->discount(coupon->date()));
                            flow_interest_builder.add_price(coupon->amount() *
                                                            discounting_term_structure->second->currentLink()->discount(coupon->date()));
                            auto flow_interest = flow_interest_builder.Finish();

                            auto flows_wrapper_builder = quantra::FlowsWrapperBuilder(*builder);
                            flows_wrapper_builder.add_flow_type(quantra::Flow_FlowInterest);
                            flows_wrapper_builder.add_flow(flow_interest.Union());
                            auto flow = flows_wrapper_builder.Finish();
                            flows_vector.push_back(flow);
                        }
                        else
                        {
                            std::ostringstream os_start_date;
                            os_start_date << QuantLib::io::iso_date(coupon->accrualStartDate());
                            auto accrual_start_date = builder->CreateString(os_start_date.str());

                            std::ostringstream os_end_date;
                            os_end_date << QuantLib::io::iso_date(coupon->accrualEndDate());
                            auto accrual_end_date = builder->CreateString(os_end_date.str());

                            auto flow_past_interest_builder = FlowPastInterestBuilder(*builder);
                            flow_past_interest_builder.add_amount(coupon->amount());
                            flow_past_interest_builder.add_fixing_date(coupon->indexFixing());
                            flow_past_interest_builder.add_accrual_start_date(accrual_start_date);
                            flow_past_interest_builder.add_accrual_end_date(accrual_end_date);
                            flow_past_interest_builder.add_rate(coupon->rate());
                            auto flow_past_interest = flow_past_interest_builder.Finish();

                            auto flows_wrapper_builder = quantra::FlowsWrapperBuilder(*builder);
                            flows_wrapper_builder.add_flow_type(quantra::Flow_FlowPastInterest);
                            flows_wrapper_builder.add_flow(flow_past_interest.Union());
                            auto flow = flows_wrapper_builder.Finish();
                            flows_vector.push_back(flow);
                        }
                    }
                    else
                    {
                        auto coupon = std::dynamic_pointer_cast<CashFlow>(*it);

                        if (!coupon->hasOccurred(Settings::instance().evaluationDate()))
                        {
                            std::ostringstream os;

                            os << QuantLib::io::iso_date(coupon->date());
                            auto date = builder->CreateString(os.str());

                            auto flow_notional_builder = FlowNotionalBuilder(*builder);
                            flow_notional_builder.add_amount(coupon->amount());
                            flow_notional_builder.add_date(date);
                            flow_notional_builder.add_discount(discounting_term_structure->second->currentLink()->discount(coupon->date()));
                            flow_notional_builder.add_price(coupon->amount() *
                                                            discounting_term_structure->second->currentLink()->discount(coupon->date()));
                            auto flow_past_interest = flow_notional_builder.Finish();

                            auto flows_wrapper_builder = quantra::FlowsWrapperBuilder(*builder);
                            flows_wrapper_builder.add_flow_type(quantra::Flow_FlowNotional);
                            flows_wrapper_builder.add_flow(flow_past_interest.Union());
                            auto flow = flows_wrapper_builder.Finish();
                            flows_vector.push_back(flow);
                        }
                    }
                }
            }

            auto flows = builder->CreateVector(flows_vector);

            FloatingRateBondResponseBuilder response_builder(*builder);

            response_builder.add_flows(flows);
            response_builder.add_npv(bond->NPV());

            if (pricing->bond_pricing_details)
            {
                YieldParser yield_parser = YieldParser();
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
    }
    auto bonds = builder->CreateVector(bonds_vector);
    PriceFloatingRateBondResponseBuilder response_builder(*builder);
    response_builder.add_bonds(bonds);

    return response_builder.Finish();
}