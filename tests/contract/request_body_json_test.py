"""Request bodies must be a single JSON object.

The gateway decodes a body with the FlatBuffers schema parser. On its own that
parser also reads schema statements (include, namespace, table, root_type,
attribute, ...) in front of a JSON object, and skips comments anywhere. Neither
is JSON, and a statement would stay in the gateway thread's parser and change
how later bodies are decoded. So a body must be exactly one JSON object: text
before the opening brace and comments are rejected with HTTP 400, and the
following requests on the same kept-alive connection (served by the same
gateway thread) decode exactly as before. Whitespace around the object and
any text inside strings are unaffected.
"""

import http.client
import json
from urllib.parse import urlsplit

import pytest
import requests

ROUTE = "/calendar-business-days"
RULE = "single JSON object"


@pytest.fixture(scope="module")
def valid_body(data_dir):
    with open(data_dir / "calendar/cal_bd_target_easter_2025.json") as fh:
        return json.dumps(json.load(fh))


@pytest.fixture
def conn(client):
    url = urlsplit(client.base_url)
    connection = http.client.HTTPConnection(url.hostname, url.port or 80, timeout=30)
    yield connection
    connection.close()


def _post(conn, body, route=ROUTE):
    if isinstance(body, str):
        body = body.encode("utf-8")
    conn.request("POST", route, body=body, headers={"Content-Type": "application/json"})
    response = conn.getresponse()
    return response.status, response.read()


def _error(payload):
    return json.loads(payload).get("error", "")


@pytest.mark.parametrize(
    "prefix",
    [
        "table X { a:int; }\n",
        "table X { a:int; }\nroot_type X;\n",
        'include "enums.fbs";\n',
    ],
    ids=["table", "table-and-root_type", "include"],
)
def test_schema_statements_before_the_object_are_rejected(conn, valid_body, prefix):
    status, reference = _post(conn, valid_body)
    assert status == 200, f"reference: HTTP {status} :: {reference[:200]!r}"
    for attempt in range(3):
        status, payload = _post(conn, prefix + valid_body)
        assert status == 400, (
            f"attempt {attempt}: expected HTTP 400, got {status} :: {payload[:200]!r}"
        )
        error = _error(payload)
        # Same connection, so the same gateway thread and parser.
        status, after = _post(conn, valid_body)
        assert status == 200, (
            f"attempt {attempt}: valid body afterwards: HTTP {status} :: {after[:200]!r}"
        )
        assert after == reference, f"attempt {attempt}: result changed"
        assert RULE in error, error


def test_statement_only_body_is_rejected_and_the_gateway_keeps_serving(
    client, conn, valid_body
):
    status, reference = _post(conn, valid_body)
    assert status == 200, f"reference: HTTP {status} :: {reference[:200]!r}"

    status, payload = _post(conn, 'attribute "x";')
    assert status == 400, f"expected HTTP 400, got {status} :: {payload[:200]!r}"
    assert RULE in _error(payload), payload[:200]

    health = requests.get(f"{client.base_url}/health", timeout=10)
    assert health.status_code == 200
    status, payload = _post(conn, valid_body)
    assert status == 200, f"HTTP {status} :: {payload[:200]!r}"
    assert payload == reference


@pytest.mark.parametrize(
    "make_body",
    [
        lambda body: "// note\n" + body,
        lambda body: "{ /* note */ " + body[1:],
        lambda body: body + "\n// note\n",
    ],
    ids=["leading-line-comment", "block-comment-inside", "trailing-line-comment"],
)
def test_comments_are_rejected(conn, valid_body, make_body):
    status, payload = _post(conn, make_body(valid_body))
    assert status == 400, f"expected HTTP 400, got {status} :: {payload[:200]!r}"
    error = _error(payload)
    assert RULE in error and "comment" in error, payload[:200]


@pytest.mark.parametrize(
    "wrap",
    [
        lambda body: " \t\r\n" + body + "\r\n \t",
        lambda body: "﻿" + body,
    ],
    ids=["json-whitespace", "byte-order-mark"],
)
def test_whitespace_around_the_object_is_accepted(conn, valid_body, wrap):
    status, reference = _post(conn, valid_body)
    assert status == 200, f"reference: HTTP {status} :: {reference[:200]!r}"
    status, payload = _post(conn, wrap(valid_body))
    assert status == 200, f"HTTP {status} :: {payload[:200]!r}"
    assert payload == reference


def test_slashes_and_quotes_inside_strings_are_data(client, conn, data_dir):
    # A curve id is free text: comment markers and quotes inside a string
    # are not comments.
    with open(data_dir / "bonds/fixed_rate_bond_request.json") as fh:
        request = json.load(fh)
    route = "/" + client.ENDPOINTS["fixed_rate_bond"]
    status, reference = _post(conn, json.dumps(request), route)
    assert status == 200, f"reference: HTTP {status} :: {reference[:200]!r}"

    curve_id = request["bonds"][0]["discounting_curve"]
    renamed = "it's \"//not\" /* a comment */"
    body = json.dumps(request).replace(json.dumps(curve_id), json.dumps(renamed))
    assert body.count(json.dumps(renamed)) == 2, "expected the curve and its reference"
    status, payload = _post(conn, body, route)
    assert status == 200, f"HTTP {status} :: {payload[:200]!r}"
    assert json.loads(payload) == json.loads(reference)
