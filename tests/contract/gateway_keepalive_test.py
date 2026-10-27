"""Gateway keep-alive latency contract.

A client that reuses one HTTP/1.1 connection must get each response as soon as
it is computed. The gateway writes a response as many small buffers, which
leave in more than one TCP segment, so its sockets run with Nagle's algorithm
disabled (TCP_NODELAY). Without that, the last segment of every response waits
for the client's delayed ACK: about 40 ms per request on a kept-alive
connection, against well under a millisecond for a cheap route.
"""

import http.client
import json
import statistics
import time
from urllib.parse import urlsplit

import pytest

ROUTE = "/calendar-business-days"
TIMED_REQUESTS = 5
MEDIAN_LIMIT_MS = 20.0  # the delayed-ACK stall is ~40 ms per request


@pytest.fixture(scope="module")
def calendar_body(data_dir):
    with open(data_dir / "calendar/cal_bd_target_easter_2025.json") as fh:
        return json.dumps(json.load(fh)).encode()


def _post(conn, body):
    conn.request("POST", ROUTE, body=body, headers={"Content-Type": "application/json"})
    response = conn.getresponse()
    return response.status, response.read()


def test_posts_on_a_kept_alive_connection_are_not_delayed(client, calendar_body):
    url = urlsplit(client.base_url)
    conn = http.client.HTTPConnection(url.hostname, url.port or 80, timeout=10)
    try:
        # Warm-up: connection set-up and first-request costs stay out of the
        # timed requests.
        status, payload = _post(conn, calendar_body)
        assert status == 200, f"warm-up: HTTP {status} :: {payload[:200]!r}"
        sock = conn.sock

        elapsed_ms = []
        for _ in range(TIMED_REQUESTS):
            start = time.perf_counter()
            status, payload = _post(conn, calendar_body)
            elapsed_ms.append((time.perf_counter() - start) * 1000.0)
            assert status == 200, f"HTTP {status} :: {payload[:200]!r}"

        assert conn.sock is sock, "the gateway did not keep the connection open"
        median_ms = statistics.median(elapsed_ms)
        assert median_ms < MEDIAN_LIMIT_MS, (
            f"median {median_ms:.1f} ms per POST on a kept-alive connection "
            f"(limit {MEDIAN_LIMIT_MS} ms; samples "
            f"{', '.join(f'{ms:.1f}' for ms in elapsed_ms)} ms)"
        )
    finally:
        conn.close()
