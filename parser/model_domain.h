#ifndef QUANTRA_MODEL_DOMAIN_H
#define QUANTRA_MODEL_DOMAIN_H

/**
 * Plain-domain (FB-free) representation of a ModelSpec.
 *
 * Mirror of flatbuffers/fbs/model.fbs:ModelSpec, expressed as
 * std::variant<CapFloorModelDomain, SwaptionModelDomain, CdsModelDomain,
 * EquityVanillaModelDomain> so consumers (CDS pricer, Swaption pricer,
 * future) need not include any *_generated.h header. Populated by
 * PricingRegistryBuilder alongside the legacy FB pointer.
 *
 * The *Kind enums used below live in common/enums_domain.h, where they are
 * value-locked to their FlatBuffers counterparts. Where a field has a natural
 * QuantLib type (Period vectors in the HW calibration spec), the QL type is
 * used directly.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <ql/time/period.hpp>

#include "enums_domain.h"

namespace quantra {

struct CapFloorModelDomain {
    IrModelTypeKind model_type = IrModelTypeKind::Black;
};

struct SwaptionHwCalibrationDomain {
    std::string swaption_vol_id;
    std::string discount_curve_id;
    std::string forwarding_curve_id;
    std::string swap_index_id;
    /// Optional grid overrides. Empty vector means "fall back to ATM matrix".
    std::vector<QuantLib::Period> expiries;
    std::vector<QuantLib::Period> tenors;
    bool calibrate_a = true;
    bool calibrate_sigma = true;
    double a_init = 0.03;
    double sigma_init = 0.01;
    int max_iterations = 200;
    int function_evaluations = 1000;
    double end_criteria_eps = 1.0e-8;
};

struct SwaptionModelDomain {
    IrModelTypeKind model_type = IrModelTypeKind::Black;
    double hw_a = 0.03;
    double hw_sigma = 0.01;
    int lattice_steps = 50;
    ModelParamModeKind param_mode = ModelParamModeKind::Explicit;
    std::optional<SwaptionHwCalibrationDomain> hw_calibration;
};

struct CdsModelDomain {
    CdsEngineTypeKind engine_type = CdsEngineTypeKind::MidPoint;
    bool include_settlement_date_flows = false;
    CdsIsdaNumericalFixKind isda_numerical_fix = CdsIsdaNumericalFixKind::Taylor;
    CdsIsdaAccrualBiasKind isda_accrual_bias = CdsIsdaAccrualBiasKind::HalfDayBias;
    CdsIsdaForwardsInCouponPeriodKind isda_forwards_in_coupon_period =
        CdsIsdaForwardsInCouponPeriodKind::Piecewise;
};

struct EquityVanillaModelDomain {
    EquityModelTypeKind model_type = EquityModelTypeKind::BlackScholesAnalytic;
    int binomial_steps = 500;
};

using ModelPayloadDomain = std::variant<
    CapFloorModelDomain,
    SwaptionModelDomain,
    CdsModelDomain,
    EquityVanillaModelDomain>;

struct ModelDomain {
    std::string id;
    ModelPayloadDomain payload;
};

} // namespace quantra

#endif // QUANTRA_MODEL_DOMAIN_H
