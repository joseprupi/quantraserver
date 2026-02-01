#include "flatbuffers/grpc.h"

#include "quantra_structs.h"
#include "term_structure_generated.h"
#include "index_generated.h"
#include "common_generated.h"
#include "fixed_rate_bond_generated.h"
#include "floating_rate_bond_generated.h"
#include "price_fixed_rate_bond_request_generated.h"
#include "price_floating_rate_bond_request_generated.h"

using namespace structs;

flatbuffers::Offset<quantra::Yield> yield_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::Yield> yield);
flatbuffers::Offset<quantra::Pricing> pricing_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::Pricing> pricing);
flatbuffers::Offset<quantra::DepositHelper> deposit_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::DepositHelper> deposit);
flatbuffers::Offset<quantra::Schedule> schedule_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::Schedule> schedule);
flatbuffers::Offset<quantra::Index> index_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::Index> index);
flatbuffers::Offset<quantra::PointsWrapper> deposit_helper_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::DepositHelper> deposit_helper);
flatbuffers::Offset<quantra::PointsWrapper> FRAhelper_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::FRAHelper> FRA_helper);
flatbuffers::Offset<quantra::PointsWrapper> future_helper_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::FutureHelper> future_helper);
flatbuffers::Offset<quantra::PointsWrapper> swap_helper_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::SwapHelper> swap_helper);
flatbuffers::Offset<quantra::PointsWrapper> bond_helper_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::BondHelper> bond_helper);
flatbuffers::Offset<quantra::TermStructure> term_structure_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::TermStructure> term_structure);
flatbuffers::Offset<quantra::FixedRateBond> fixed_rate_bond_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::FixedRateBond> fixed_rate_bond);
flatbuffers::Offset<quantra::PriceFixedRateBond> price_fixed_rate_bond_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::PriceFixedRateBond> price_bond);
flatbuffers::Offset<quantra::PriceFixedRateBondRequest> price_fixed_rate_bond_request_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::PriceFixedRateBondRequest> price_bond);
flatbuffers::Offset<quantra::FloatingRateBond> floating_rate_bond_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::FloatingRateBond> floating_rate_bond);
flatbuffers::Offset<quantra::PriceFloatingRateBond> price_floating_rate_bond_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::PriceFloatingRateBond> price_bond);
flatbuffers::Offset<quantra::PriceFloatingRateBondRequest> price_floating_rate_bond_request_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::PriceFloatingRateBondRequest> price_bond);