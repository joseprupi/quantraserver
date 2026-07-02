"""Gateway DX contract.

Locks the Portal-facing HTTP error/observability behaviour of the JSON
gateway:

  * X-Request-Id: the caller's id is echoed back as a response header and
    forwarded to the gRPC engine as `x-request-id` metadata, so the engine
    log line for the request carries `request_id=<id>`. The id is
    sanitized (printable ASCII only) and capped at 128 chars.
  * ABORTED (QuantLib/engine exception from a well-formed but unpriceable
    request) maps to HTTP 422 -- not the old default 500 -- and the
    documented `error` field carries the REAL cause text, with `code`,
    `code_name` and `message` kept for backward compatibility.
  * An empty/whitespace-only body is rejected up front with a clear
    400 "empty request body" instead of flatc's "input file is empty",
    and a body with an embedded NUL byte is rejected instead of being
    silently truncated at the NUL.
  * API version visibility: the top-level VERSION file is the single source
    of truth, surfaced as the X-Quantra-Api-Version response header, /meta's
    api_version field, and the committed OpenAPI spec's info.version.

The engine log written by run_all_tests.sh (/tmp/grpc.log) is used for the
log-correlation assertion when it is reachable; otherwise only the response
header echo is asserted.
"""

import json
import os
import re
import uuid
from pathlib import Path

import pytest
import requests

GRPC_LOG = Path(os.environ.get("QUANTRA_GRPC_LOG", "/tmp/grpc.log"))
JSON_HEADERS = {"Content-Type": "application/json"}

WORKSPACE = Path(__file__).resolve().parents[2]
EXPECTED_API_VERSION = (WORKSPACE / "VERSION").read_text().strip()


@pytest.fixture(scope="module")
def bond_url(client):
    return f"{client.base_url}/{client.ENDPOINTS['fixed_rate_bond']}"


@pytest.fixture(scope="module")
def bond_request(data_dir):
    with open(data_dir / "fixed_rate_bond_request.json") as fh:
        return json.load(fh)


# ---------------------------------------------------------------------------
# X-Request-Id forwarding + echo
# ---------------------------------------------------------------------------

def test_request_id_echoed_on_response(bond_url, bond_request):
    r = requests.post(
        bond_url,
        data=json.dumps(bond_request),
        headers={**JSON_HEADERS, "X-Request-Id": "probe-123"},
    )
    assert r.status_code == 200, f"expected 200, got {r.status_code} :: {r.text[:160]}"
    assert r.headers.get("X-Request-Id") == "probe-123", (
        f"X-Request-Id not echoed: headers={dict(r.headers)}"
    )


def test_request_id_reaches_engine_log(bond_url, bond_request):
    # Force an engine log line (errors always log) with a unique id, then look
    # for request_id=<id> in the engine's log. The mutation below produces a
    # deterministic QuantLib error (effective date after termination date).
    rid = f"probe-{uuid.uuid4().hex[:12]}"
    bad = json.loads(json.dumps(bond_request))
    bad["bonds"][0]["fixed_rate_bond"]["schedule"]["effective_date"] = "2018/05/15"

    r = requests.post(
        bond_url,
        data=json.dumps(bad),
        headers={**JSON_HEADERS, "X-Request-Id": rid},
    )
    assert r.headers.get("X-Request-Id") == rid, (
        f"X-Request-Id not echoed on error response: headers={dict(r.headers)}"
    )
    if not GRPC_LOG.is_file():
        pytest.skip(f"engine log {GRPC_LOG} not reachable; header echo asserted above")
    log_text = GRPC_LOG.read_text(errors="replace")
    assert f"request_id={rid}" in log_text, (
        f"engine log has no request_id={rid} line (request-id forwarding broken)"
    )


def test_request_id_sanitized_and_capped(bond_url, bond_request):
    # 200 printable chars in -> at most 128 echoed back (gateway cap).
    long_id = "a" * 200
    r = requests.post(
        bond_url,
        data=json.dumps(bond_request),
        headers={**JSON_HEADERS, "X-Request-Id": long_id},
    )
    echoed = r.headers.get("X-Request-Id")
    assert echoed == "a" * 128, f"expected 128-char capped id, got {echoed!r}"


def test_no_request_id_header_when_absent(bond_url, bond_request):
    r = requests.post(bond_url, data=json.dumps(bond_request), headers=JSON_HEADERS)
    assert r.status_code == 200
    assert "X-Request-Id" not in r.headers, (
        "gateway must not invent a request id when the caller sent none"
    )


# ---------------------------------------------------------------------------
# ABORTED -> 422 with the real cause in `error`
# ---------------------------------------------------------------------------

def test_quantlib_error_maps_to_422_with_real_cause(bond_url, bond_request):
    bad = json.loads(json.dumps(bond_request))
    # Well-formed but unpriceable: schedule effective date after termination
    # date makes QuantLib throw -> gRPC ABORTED -> HTTP 422 (was 500).
    bad["bonds"][0]["fixed_rate_bond"]["schedule"]["effective_date"] = "2018/05/15"

    r = requests.post(bond_url, data=json.dumps(bad), headers=JSON_HEADERS)
    assert r.status_code == 422, (
        f"QuantLib input error: expected HTTP 422, got {r.status_code} :: {r.text[:200]}"
    )
    body = r.json()
    # The documented `error` field carries the real message, not "gRPC error".
    assert body.get("error") and body["error"] != "gRPC error", (
        f"'error' field is not the real cause: {r.text[:200]}"
    )
    assert "date" in body["error"].lower(), (
        f"'error' does not mention the offending dates: {r.text[:200]}"
    )
    # Backward compatibility: code/code_name/message are still present.
    assert body.get("code_name") == "ABORTED", f"unexpected code_name: {r.text[:200]}"
    assert isinstance(body.get("code"), int)
    assert body.get("message") == body["error"], (
        f"'message' kept for compat must match 'error': {r.text[:200]}"
    )


# ---------------------------------------------------------------------------
# empty body + NUL byte handling
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("body", [b"", b"   \n\t  "], ids=["empty", "whitespace-only"])
def test_empty_body_rejected_400(bond_url, body):
    r = requests.post(bond_url, data=body, headers=JSON_HEADERS)
    assert r.status_code == 400, (
        f"empty body: expected HTTP 400, got {r.status_code} :: {r.text[:160]}"
    )
    assert r.json() == {"error": "empty request body"}


def test_body_with_embedded_nul_rejected_400(bond_url, bond_request):
    # A NUL inside the body used to silently truncate the request at the NUL
    # (the body is handed to flatc as a C string); it must be rejected.
    body = json.dumps(bond_request).encode() + b"\x00trailing"
    r = requests.post(bond_url, data=body, headers=JSON_HEADERS)
    assert r.status_code == 400, (
        f"NUL body: expected HTTP 400, got {r.status_code} :: {r.text[:160]}"
    )
    assert "NUL" in r.json().get("error", ""), (
        f"expected a clear NUL-byte error, got: {r.text[:160]}"
    )


# ---------------------------------------------------------------------------
# API version visibility (single source of truth = VERSION file)
# ---------------------------------------------------------------------------

def test_api_version_header_on_pricing_response(bond_url, bond_request):
    r = requests.post(bond_url, data=json.dumps(bond_request), headers=JSON_HEADERS)
    assert r.status_code == 200, f"expected 200, got {r.status_code} :: {r.text[:160]}"
    assert r.headers.get("X-Quantra-Api-Version") == EXPECTED_API_VERSION, (
        f"X-Quantra-Api-Version != VERSION file ({EXPECTED_API_VERSION}): "
        f"headers={dict(r.headers)}"
    )


def test_api_version_header_on_error_response(bond_url):
    # The version must be detectable even when the request is rejected.
    r = requests.post(bond_url, data=b"", headers=JSON_HEADERS)
    assert r.status_code == 400
    assert r.headers.get("X-Quantra-Api-Version") == EXPECTED_API_VERSION, (
        f"X-Quantra-Api-Version missing on error response: headers={dict(r.headers)}"
    )


def test_meta_reports_version_from_version_file(client):
    r = requests.get(f"{client.base_url}/meta")
    assert r.status_code == 200
    body = r.json()
    assert body.get("api_version") == EXPECTED_API_VERSION, (
        f"/meta api_version != VERSION file ({EXPECTED_API_VERSION}): {r.text[:200]}"
    )


def test_openapi_spec_version_and_response_ref():
    # Guards regeneration drift: the committed spec must carry the VERSION
    # file's version, and a pricing endpoint's documented 200 body must be the
    # response WRAPPER type (the .fbs root_type), not the inner item type.
    spec_path = WORKSPACE / "jsonserver" / "openapi" / "openapi3.json"
    with open(spec_path) as fh:
        spec = json.load(fh)
    assert spec["info"]["version"] == EXPECTED_API_VERSION, (
        f"OpenAPI info.version {spec['info']['version']!r} != VERSION file "
        f"({EXPECTED_API_VERSION})"
    )

    fbs = (WORKSPACE / "flatbuffers" / "fbs" / "fixed_rate_bond_response.fbs").read_text()
    match = re.search(r"^\s*root_type\s+(\w+)\s*;", fbs, re.MULTILINE)
    assert match, "no root_type found in fixed_rate_bond_response.fbs"
    expected_ref = f"#/components/schemas/quantra_{match.group(1)}"

    responses = spec["paths"]["/price-fixed-rate-bond"]["post"]["responses"]
    actual_ref = responses["200"]["content"]["application/json"]["schema"]["$ref"]
    assert actual_ref == expected_ref, (
        f"documented 200 body {actual_ref} != wire root {expected_ref}"
    )
