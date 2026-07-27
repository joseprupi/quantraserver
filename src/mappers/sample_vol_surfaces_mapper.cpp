#include "sample_vol_surfaces_mapper.h"

#include <vector>

#include "date_convert.h"
#include "enum_convert.h"
#include "require_field.h"
#include "error.h"
#include "swaption_vol_diagnostics.h"

namespace quantra {

namespace {

SampleSurfaceType toSampleSurfaceType(quantra::VolSurfaceType t) {
    switch (t) {
    case quantra::VolSurfaceType_Swaption:
        return SampleSurfaceType::Swaption;
    case quantra::VolSurfaceType_Optionlet:
        return SampleSurfaceType::Optionlet;
    case quantra::VolSurfaceType_EquityBlack:
        return SampleSurfaceType::EquityBlack;
    }
    return SampleSurfaceType::Swaption;
}

SampleStrikeAxis toSampleStrikeAxis(quantra::VolStrikeAxis a) {
    return a == quantra::VolStrikeAxis_SpreadFromATM ? SampleStrikeAxis::SpreadFromATM
                                                     : SampleStrikeAxis::AbsoluteStrike;
}

SampleOutputMode toSampleOutputMode(quantra::VolOutputMode m) {
    switch (m) {
    case quantra::VolOutputMode_Cube:
        return SampleOutputMode::Cube;
    case quantra::VolOutputMode_SmileSlice:
        return SampleOutputMode::SmileSlice;
    case quantra::VolOutputMode_TermSlice:
        return SampleOutputMode::TermSlice;
    case quantra::VolOutputMode_ExpirySlice:
        return SampleOutputMode::ExpirySlice;
    }
    return SampleOutputMode::Cube;
}

quantra::VolStrikeAxis toFbStrikeAxis(SampleStrikeAxis a) {
    return a == SampleStrikeAxis::SpreadFromATM ? quantra::VolStrikeAxis_SpreadFromATM
                                                : quantra::VolStrikeAxis_AbsoluteStrike;
}

quantra::ExpiryKind toFbExpiryKind(SampleExpiryKind k) {
    return k == SampleExpiryKind::ExerciseDate ? quantra::ExpiryKind_ExerciseDate
                                               : quantra::ExpiryKind_GridDate;
}

SampleQueryOptions extractOptions(const quantra::QueryOptions* o) {
    SampleQueryOptions out;
    if (!o) return out;
    out.present = true;
    out.calendar = o->calendar();
    out.businessDayConvention = o->business_day_convention();
    out.maxPoints = o->max_points();
    out.allowExtrapolation = o->allow_extrapolation();
    out.strict = o->strict();
    return out;
}

SampleDateGridSpec extractGridSpec(const quantra::DateGridSpec* spec) {
    SampleDateGridSpec out;
    if (!spec) return out;
    out.present = true;
    if (spec->grid_type() == quantra::DateGrid_TenorGrid) {
        out.kind = SampleDateGridKind::Tenor;
        const auto* g = spec->grid_as_TenorGrid();
        if (g) {
            out.tenor.calendar = g->calendar();
            out.tenor.businessDayConvention = g->business_day_convention();
            if (g->tenors()) {
                out.tenor.hasTenors = true;
                out.tenor.tenors.reserve(g->tenors()->size());
                for (flatbuffers::uoffset_t i = 0; i < g->tenors()->size(); ++i) {
                    const auto* t = g->tenors()->Get(i);
                    SampleTenor st;
                    st.n = requireInt(t->n(), "SwaptionTenorGrid.tenors.n");
                    st.unit = requireEnum(t->unit(), "SwaptionTenorGrid.tenors.unit");
                    out.tenor.tenors.push_back(st);
                }
            }
        }
    } else if (spec->grid_type() == quantra::DateGrid_RangeGrid) {
        out.kind = SampleDateGridKind::Range;
        const auto* g = spec->grid_as_RangeGrid();
        if (g) {
            if (g->start_date()) {
                out.range.hasStartDate = true;
                out.range.startDate = g->start_date()->str();
            }
            if (g->end_date()) {
                out.range.hasEndDate = true;
                out.range.endDate = g->end_date()->str();
            }
            out.range.stepNumber = g->step_number();
            out.range.stepTimeUnit = g->step_time_unit();
            out.range.businessDaysOnly = g->business_days_only();
            out.range.calendar = g->calendar();
            out.range.businessDayConvention = g->business_day_convention();
        }
    } else {
        out.kind = SampleDateGridKind::None;
    }
    return out;
}

SampleVolSurfacesQuery extractQuery(const quantra::VolQuerySpec* q) {
    SampleVolSurfacesQuery out;
    out.volId = (q && q->vol_id()) ? q->vol_id()->str() : std::string{};
    out.surfaceType = toSampleSurfaceType(q->surface_type());

    if (q->strike_grid()) {
        out.hasStrikeGrid = true;
        out.strikeAxis = toSampleStrikeAxis(q->strike_grid()->axis());
        if (q->strike_grid()->strikes()) {
            out.hasStrikes = true;
            const auto* s = q->strike_grid()->strikes();
            out.strikes.reserve(s->size());
            for (flatbuffers::uoffset_t i = 0; i < s->size(); ++i) {
                out.strikes.push_back(s->Get(i));
            }
        }
    }

    out.expiryGrid = extractGridSpec(q->expiry_grid());
    out.tenorGrid = extractGridSpec(q->tenor_grid());
    out.options = extractOptions(q->options());
    out.outputMode = toSampleOutputMode(q->output_mode());

    if (q->swap_index_id()) {
        out.hasSwapIndexId = true;
        out.swapIndexId = q->swap_index_id()->str();
    }

    out.sliceExpiryIndex = q->slice_expiry_index();
    out.sliceTenorIndex = q->slice_tenor_index();
    out.sliceStrike = q->slice_strike();
    out.sliceStrikeIsSet = q->slice_strike_is_set();

    if (q->discounting_curve_id()) {
        out.hasDiscountingCurveId = true;
        out.discountingCurveId = q->discounting_curve_id()->str();
    }
    if (q->forwarding_curve_id()) {
        out.hasForwardingCurveId = true;
        out.forwardingCurveId = q->forwarding_curve_id()->str();
    }
    return out;
}

flatbuffers::Offset<quantra::Period> toFbPeriod(
    flatbuffers::grpc::MessageBuilder& b, const QuantLib::Period& p) {
    quantra::PeriodBuilder pb(b);
    pb.add_n(p.length());
    pb.add_unit(TimeUnitToFb(p.units()));
    return pb.Finish();
}

} // namespace

SampleVolSurfacesInputs SampleVolSurfacesMapper::toInputs(
    const quantra::SampleVolSurfacesRequest* req) const {
    if (!req || !req->pricing() || !req->queries() || req->queries()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("SampleVolSurfacesRequest.pricing and non-empty queries are required");
    }
    SampleVolSurfacesInputs inputs;
    inputs.includeDiagnostics = req->include_diagnostics();
    inputs.queries.reserve(req->queries()->size());
    for (flatbuffers::uoffset_t qi = 0; qi < req->queries()->size(); ++qi) {
        inputs.queries.push_back(extractQuery(req->queries()->Get(qi)));
    }
    return inputs;
}

flatbuffers::Offset<quantra::SampleVolSurfacesResponse>
SampleVolSurfacesMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const SampleVolSurfacesResult& result) const {
    std::vector<flatbuffers::Offset<quantra::VolSurfaceSample>> results;
    results.reserve(result.samples.size());

    for (const auto& s : result.samples) {
        if (s.hasError) {
            auto volIdOffset = builder.CreateString(s.volId);
            auto errMsg = builder.CreateString(s.error);
            quantra::ErrorBuilder eb(builder);
            eb.add_error_message(errMsg);
            auto errOffset = eb.Finish();

            quantra::VolSurfaceSampleBuilder out(builder);
            out.add_vol_id(volIdOffset);
            out.add_error(errOffset);
            results.push_back(out.Finish());
            continue;
        }

        auto volIdOffset = builder.CreateString(s.volId);
        auto refDateOffset = builder.CreateString(DateToIso(s.referenceDate));

        std::vector<flatbuffers::Offset<flatbuffers::String>> expiriesStr;
        expiriesStr.reserve(s.expiries.size());
        for (const auto& d : s.expiries) expiriesStr.push_back(builder.CreateString(DateToIso(d)));

        std::vector<flatbuffers::Offset<flatbuffers::String>> requestedExpiryStr;
        requestedExpiryStr.reserve(s.requestedExpiryGridPoints.size());
        for (const auto& d : s.requestedExpiryGridPoints)
            requestedExpiryStr.push_back(builder.CreateString(DateToIso(d)));

        std::vector<flatbuffers::Offset<quantra::Period>> tenorOffs;
        tenorOffs.reserve(s.tenors.size());
        for (const auto& p : s.tenors) tenorOffs.push_back(toFbPeriod(builder, p));

        std::vector<flatbuffers::Offset<flatbuffers::String>> effectiveStartsStr;
        effectiveStartsStr.reserve(s.effectiveSwapStarts.size());
        for (const auto& d : s.effectiveSwapStarts)
            effectiveStartsStr.push_back(builder.CreateString(DateToIso(d)));

        std::vector<flatbuffers::Offset<flatbuffers::String>> effectiveEndsStr;
        effectiveEndsStr.reserve(s.effectiveSwapEnds.size());
        for (const auto& d : s.effectiveSwapEnds)
            effectiveEndsStr.push_back(builder.CreateString(DateToIso(d)));

        auto expiriesVec = builder.CreateVector(expiriesStr);
        auto requestedExpiriesVec = builder.CreateVector(requestedExpiryStr);
        auto tenorsVec = builder.CreateVector(tenorOffs);
        auto effectiveStartsVec = builder.CreateVector(effectiveStartsStr);
        auto effectiveEndsVec = builder.CreateVector(effectiveEndsStr);
        auto strikesVec = builder.CreateVector(s.strikes);
        auto volsVec = builder.CreateVector(s.vols);
        auto atmVec = builder.CreateVector(s.atmLevels);

        quantra::VolSurfaceSampleBuilder out(builder);
        out.add_vol_id(volIdOffset);
        out.add_reference_date(refDateOffset);
        out.add_ql_vol_type(s.qlVolType);
        out.add_requested_strike_axis(toFbStrikeAxis(s.requestedStrikeAxis));
        out.add_canonical_strike_kind(s.canonicalStrikeKind);
        out.add_allow_extrapolation_used(s.allowExtrapolationUsed);
        out.add_calendar_used(s.calendarUsed);
        out.add_business_day_convention_used(s.businessDayConventionUsed);
        out.add_expiry_kind(toFbExpiryKind(s.expiryKind));
        out.add_expiries(expiriesVec);
        out.add_requested_expiry_grid_points(requestedExpiriesVec);
        out.add_tenors(tenorsVec);
        out.add_effective_swap_starts(effectiveStartsVec);
        out.add_effective_swap_ends(effectiveEndsVec);
        out.add_strikes(strikesVec);
        out.add_vols(volsVec);
        out.add_n_expiries(s.nExpiries);
        out.add_n_tenors(s.nTenors);
        out.add_n_strikes(s.nStrikes);
        out.add_atm_levels(atmVec);
        results.push_back(out.Finish());
    }

    std::vector<flatbuffers::Offset<quantra::SwaptionVolDiagnostics>> diagnosticsOffs;
    diagnosticsOffs.reserve(result.diagnostics.size());
    for (const auto& d : result.diagnostics) {
        if (!d.partial) {
            diagnosticsOffs.push_back(
                buildSwaptionVolDiagnostics(builder, d.volId, d.finalized, d.warnings));
        } else {
            diagnosticsOffs.push_back(
                buildPartialSwaptionVolDiagnostics(builder, d.volId, d.kind, d.warnings));
        }
    }

    auto resultsVec = builder.CreateVector(results);
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwaptionVolDiagnostics>>> diagVec = 0;
    if (!diagnosticsOffs.empty()) {
        diagVec = builder.CreateVector(diagnosticsOffs);
    }
    quantra::SampleVolSurfacesResponseBuilder rb(builder);
    rb.add_results(resultsVec);
    if (diagVec.o != 0) rb.add_diagnostics(diagVec);
    return rb.Finish();
}

void SampleVolSurfacesMapper::onRegistryBuildError(
    SampleVolSurfacesInputs& inputs, const std::string& message) const {
    for (auto& q : inputs.queries) {
        if (q.prebuiltError.empty()) {
            q.prebuiltError = message;
        }
    }
}

} // namespace quantra
