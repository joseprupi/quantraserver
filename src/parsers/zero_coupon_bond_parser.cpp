#include "zero_coupon_bond_parser.h"

#include "error.h"
#include "request_validation.h"

std::shared_ptr<QuantLib::Bond> ZeroCouponBondParser::parse(const quantra::ZeroCouponBond *bond)
{
    if (bond == NULL)
        QUANTRA_INVALID_ARGUMENT("Zero Coupon Bond not found");

    if (!bond->calendar().has_value())
        QUANTRA_INVALID_ARGUMENT("ZeroCouponBond.calendar is required");
    if (!bond->payment_convention().has_value())
        QUANTRA_INVALID_ARGUMENT("ZeroCouponBond.payment_convention is required");
    if (!bond->maturity_date())
        QUANTRA_INVALID_ARGUMENT("ZeroCouponBond.maturity_date is required");

    auto calendar = CalendarToQL(bond->calendar().value());
    auto paymentConvention = ConventionToQL(bond->payment_convention().value());
    auto maturityDate = DateToQL(bond->maturity_date()->str());

    // Redemption is optional: presence decides. Absent => QuantLib's 100.0 (par)
    // default, matching the ZeroCouponBond constructor default.
    const Real redemption = bond->redemption().has_value()
                                ? static_cast<Real>(bond->redemption().value())
                                : 100.0;

    // issue_date is optional: absent => QuantLib's null Date default.
    const Date issueDate = bond->issue_date()
                               ? DateToQL(bond->issue_date()->str())
                               : Date();

    return std::make_shared<QuantLib::ZeroCouponBond>(
        bond->settlement_days(),
        calendar,
        quantra::requirePositive(bond->face_amount(), "ZeroCouponBond.face_amount"),
        maturityDate,
        paymentConvention,
        redemption,
        issueDate);
}
