#ifndef QUANTRA_CORRELATION_CALIBRATION_MAPPER_H
#define QUANTRA_CORRELATION_CALIBRATION_MAPPER_H

/**
 * CorrelationCalibrationMapper — the one place this product's flatbuffers
 * live. Decodes a CalibrateCorrelationRequest into plain
 * CorrelationCalibrationInputs (presence checks and date parsing; the
 * estimator's rules are the evaluator's) and serializes a
 * CorrelationCalibrationResult back into a CalibrateCorrelationResponse,
 * including the ready-to-paste CorrelationMatrixSpec and the estimator echo.
 */

#include "correlation_calibration_evaluator.h"

#include "calibrate_correlation_request_generated.h"
#include "calibrate_correlation_response_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

class CorrelationCalibrationMapper {
public:
    CorrelationCalibrationInputs toInputs(const quantra::CalibrateCorrelationRequest* req) const;

    flatbuffers::Offset<quantra::CalibrateCorrelationResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const CorrelationCalibrationResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_CORRELATION_CALIBRATION_MAPPER_H
