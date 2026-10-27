#!/usr/bin/env bash
# The calls of examples/structured_note_workflow/README.md (level 2) as raw
# HTTP: curl + jq, nothing else.
#
#   price_note_from_quotes.sh [REQUEST_DIR] [URL]
#
# REQUEST_DIR holds the request bodies price_note_from_quotes.py writes
# (correlation_request.json when it ran with --closes,
# implied_forwards_<ID>_request.json, vol_surface_<ID>_request.json,
# price_note_grid_request.json, price_note_svi_request.json). Without it the
# shipped example requests are used instead: the NDX / RTY closes of
# examples/data/correlation/, the synthetic NDX-like sheet of
# examples/data/implied_forwards/ and examples/data/vol_surface/, and the
# Scotia note with its synthetic surfaces already pasted in
# (examples/data/structured_note/..._with_synthetic_surface.json), so the
# script runs from a fresh checkout. URL defaults to http://localhost:8080.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
REQ_DIR="${1:-}"
URL="${2:-http://localhost:8080}"

post() {  # post <endpoint> <request file> -> response on stdout, non-zero on HTTP error
    local endpoint="$1" file="$2" body code
    body="$(curl -sS -w '\n%{http_code}' -X POST "$URL/$endpoint" \
        -H 'Content-Type: application/json' --data-binary @"$file")"
    code="${body##*$'\n'}"
    body="${body%$'\n'*}"
    if [ "$code" != "200" ]; then
        echo "POST /$endpoint ($file) -> HTTP $code" >&2
        echo "$body" >&2
        return 1
    fi
    printf '%s\n' "$body"
}

if [ -z "$REQ_DIR" ]; then
    CORR=("$REPO/examples/data/correlation/ndx_rut_closes_2025-09-29_to_2026-09-30.json")
    FWD=("$REPO/examples/data/implied_forwards/synthetic_ndx_like_2026-09-30.json")
    VOL=("$REPO/examples/data/vol_surface/synthetic_svi_sheet_2026-09-30_svi.json")
    NOTE=("$REPO/examples/data/structured_note/scotia_autocallable_trigger_ndx_rty_us06419jme72_with_synthetic_surface.json"
          "$REPO/examples/data/structured_note/scotia_autocallable_trigger_ndx_rty_us06419jme72_with_synthetic_svi.json")
else
    CORR=()
    [ -f "$REQ_DIR/correlation_request.json" ] && CORR=("$REQ_DIR/correlation_request.json")
    FWD=("$REQ_DIR"/implied_forwards_*_request.json)
    VOL=("$REQ_DIR"/vol_surface_*_request.json)
    NOTE=("$REQ_DIR"/price_note_*_request.json)
fi

# 0. Correlation from price histories: the matrix used, the sample next to it.
for f in "${CORR[@]}"; do
    echo "== calibrate-correlation  $(basename "$f")"
    post calibrate-correlation "$f" | jq -r '
        "\(.correlation_spec.id) \(.correlation_spec.labels | join("/")): \(.observations_used) closes \(.first_date) to \(.last_date), \(.estimator_echo.returns) \(.estimator_echo.frequency), shrinkage \(.estimator_echo.shrinkage) intensity \(.shrinkage_intensity_used | . * 10000 | round / 10000)",
        "  used   \(.correlation_spec.values.values | map(. * 1000000 | round / 1000000) | join("  "))",
        "  sample \(.sample_correlation | map(. * 1000000 | round / 1000000) | join("  "))",
        (.series[] | "  \(.underlying_id): \(.observations) closes, \(.dropped) dropped, realised vol \(.annualised_volatility * 100 | . * 100 | round / 100)%")'
done

# 1. Implied forwards: forward, implied dividend yield, discount factors per expiry.
for f in "${FWD[@]}"; do
    echo "== calibrate-implied-forwards  $(basename "$f")"
    post calibrate-implied-forwards "$f" | jq -r '
        "\(.underlying_id) spot \(.spot): \(.expiries | map(select(.usable)) | length) usable expiries, dividend curve \(.dividend_curve_spec.id)",
        (.expiries[] | select(.usable) |
         "  \(.expiry)  F \(.forward | . * 1000 | round / 1000)  q \(.implied_dividend_yield * 100 | . * 1000 | round / 1000)%  DF \(.implied_discount_factor | . * 100000 | round / 100000)  used \(.quotes_used) dropped \(.quotes_dropped)")'
done

# 2. Vol surface: fit report per expiry; the grid and (SVI method) the slices.
for f in "${VOL[@]}"; do
    echo "== calibrate-vol-surface  $(basename "$f")"
    post calibrate-vol-surface "$f" | jq -r '
        "\(.underlying_id) \(.method) (forwards \(.forwards_source)): grid \(.grid_expiries | length) x \(.grid_strikes | length) \(.vol_surface_spec.payload.surface_interpolator), SVI payload \(if .vol_surface_spec_svi then "yes" else "no" end); repricing \(.vanilla_repricing.inside_bid_ask)/\(.vanilla_repricing.count) inside bid/ask, max vol error \(.vanilla_repricing.max_abs_vol_error * 100 | . * 1000 | round / 1000) pts",
        (.expiries[] | select(.usable) |
         "  \(.expiry)  used \(.quotes_used)  rmse \(.rmse_vol * 100 | . * 10000 | round / 10000) pts  butterfly \(if .butterfly_violation then "violation" else "ok" end)  calendar \(.calendar_violations) violations")'
done

# 3. The note on the surfaces, with the repricing diagnostic.
for f in "${NOTE[@]}"; do
    echo "== price-structured-note  $(basename "$f")"
    post price-structured-note "$f" | jq -r '.notes[0] |
        "\(.trade_id): \(.npv | . * 100 | round / 100) +/- \(.standard_error | . * 100 | round / 100) (\(.status), \(.paths) paths, \(.model.time_steps) steps, vol \(.model.underlyings | map(.vol_shape) | join("/")))",
        "  p_call \(.event_probabilities | map(select(.p_call != null)) | .[0].p_call // "-")  p_breach \(.p_barrier_breach // "-")  legs bond \(.legs.bond_redemption.pv | round) coupons \(.legs.coupons.pv | round) capital loss \(.legs.capital_loss.pv | round)",
        (if .vanilla_repricing then
         "  vanilla repricing: \(.vanilla_repricing.inside_bid_ask)/\(.vanilla_repricing.count) inside bid/ask, max vol error \((.vanilla_repricing.max_abs_vol_error // 0) * 100 | . * 100 | round / 100) pts, max SE \(.vanilla_repricing.max_standard_error | . * 1000 | round / 1000), note on the repricing paths \(.vanilla_repricing.note_npv | . * 100 | round / 100) +/- \(.vanilla_repricing.note_standard_error | . * 100 | round / 100)"
         else "  (no vanilla_repricing block requested)" end)'
done
