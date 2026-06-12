"""Request-body hardening contract (Content-Type check + body-size cap).

Every JSON POST route shares the gateway-side request guards: a request whose
Content-Type is not application/json (parameters such as "; charset=utf-8"
are allowed) is rejected with HTTP 415, and a request body larger than 10MB
is rejected with HTTP 413 before it is forwarded to the gRPC backend. The
contract (status + body shape) is the one the bootstrap-inflation-curves
route established:

  415 {"error":"Content-Type must be application/json"}
  413 {"error":"Request body too large"}

These scenarios drive a representative pricing route (fixed_rate_bond) with
plain requests.post (NOT the session client, whose default headers would mask
the Content-Type cases) and assert the guard fires — plus a control scenario
asserting a normal application/json request still prices.
"""

import json

import pytest
import requests

MAX_BODY_BYTES = 10 * 1024 * 1024


@pytest.fixture(scope="module")
def bond_url(client):
    return f"{client.base_url}/{client.ENDPOINTS['fixed_rate_bond']}"


@pytest.fixture(scope="module")
def bond_request(data_dir):
    with open(data_dir / "fixed_rate_bond_request.json") as fh:
        return json.load(fh)


def test_oversized_body_rejected_413(bond_url, bond_request):
    # A valid request padded past the cap: the guard must reject on size
    # alone, before any backend parsing happens.
    padded = dict(bond_request)
    padded["_padding"] = "x" * (MAX_BODY_BYTES + 1)
    body = json.dumps(padded)
    assert len(body) > MAX_BODY_BYTES

    r = requests.post(
        bond_url, data=body, headers={"Content-Type": "application/json"}
    )
    assert r.status_code == 413, (
        f"oversized body: expected HTTP 413, got {r.status_code} :: {r.text[:160]}"
    )
    assert r.json() == {"error": "Request body too large"}


def test_wrong_content_type_rejected_415(bond_url, bond_request):
    r = requests.post(
        bond_url,
        data=json.dumps(bond_request),
        headers={"Content-Type": "text/plain"},
    )
    assert r.status_code == 415, (
        f"text/plain: expected HTTP 415, got {r.status_code} :: {r.text[:160]}"
    )
    assert r.json() == {"error": "Content-Type must be application/json"}


def test_missing_content_type_rejected_415(bond_url, bond_request):
    # requests does not add a Content-Type header for raw string data; the
    # explicit None makes the omission immune to library defaults.
    r = requests.post(
        bond_url,
        data=json.dumps(bond_request),
        headers={"Content-Type": None},
    )
    assert r.status_code == 415, (
        f"no Content-Type: expected HTTP 415, got {r.status_code} :: {r.text[:160]}"
    )


def test_json_content_type_with_charset_accepted(bond_url, bond_request):
    # application/json with parameters is legitimate and must not be rejected.
    r = requests.post(
        bond_url,
        data=json.dumps(bond_request),
        headers={"Content-Type": "application/json; charset=utf-8"},
    )
    assert r.status_code == 200, (
        f"charset param: expected HTTP 200, got {r.status_code} :: {r.text[:160]}"
    )


def test_normal_request_still_prices(client, bond_request):
    response = client.price("fixed_rate_bond", bond_request)
    results = response.get("bonds")
    assert isinstance(results, list) and results, (
        f"expected non-empty 'bonds' results: {json.dumps(response)[:200]}"
    )
