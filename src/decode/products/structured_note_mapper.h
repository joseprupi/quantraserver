#ifndef QUANTRA_STRUCTURED_NOTE_MAPPER_H
#define QUANTRA_STRUCTURED_NOTE_MAPPER_H

/**
 * StructuredNoteMapper — the one place structured-note flatbuffers live.
 * Decodes a PriceStructuredNoteRequest into plain StructuredNoteInputs
 * (checking every schema rule of structured_note.fbs that needs no market
 * data: each violation is a 400 whose message starts with the field path,
 * `notes[<i>].events[<j>].payment_date: ...`) and serialises the
 * NoteResult back into a PriceStructuredNoteResponse (terms echo,
 * seasoning resolution and model echo always; legs, probabilities and the
 * standard error when something was simulated; each opt-in diagnostic block
 * when its flag was set, `diagnostics.sample_paths` capped at 100 and
 * requiring `distribution`; the Greeks are not produced in this version and
 * requesting them is a 501).
 */

#include "structured_note_evaluator.h"

#include "price_structured_note_request_generated.h"
#include "structured_note_response_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

class StructuredNoteMapper {
public:
    StructuredNoteInputs toInputs(const quantra::PriceStructuredNoteRequest* req) const;

    flatbuffers::Offset<quantra::PriceStructuredNoteResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder, const NoteResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_STRUCTURED_NOTE_MAPPER_H
