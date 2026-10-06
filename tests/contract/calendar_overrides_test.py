"""HTTP contract tests for per-request calendar holiday overrides.

Overrides are request input: they must be honoured for the request that carries
them and leave no trace afterwards. The suite runs against a single worker, so
"request with overrides, then the same request without" exercises exactly the
process-global calendar state a leak would live in.
"""

import copy
import json
import re
from pathlib import Path

import pytest

_ENUMS_FBS = Path(__file__).resolve().parents[2] / "flatbuffers" / "fbs" / "enums.fbs"

# The calendars QuantLib gives per-instance state; an override would be inert.
INERT_CALENDARS = {"BespokeCalendar", "NullCalendar"}

SWAP_FILE = "vanilla_swap_multicurve_request.json"       # as_of 2024-01-15, TARGET
BOOTSTRAP_FILE = "bootstrap_curves_tenor_grid.json"      # as_of 2025-01-15, TARGET
# Holidays on coupon roll dates (swap) and on the spot date (curves), so
# payment dates and curve pillars move. The swap override leaves the first
# fixing date alone: moving it before the as-of date would need a past fixing.
SWAP_OVERRIDES = [{"calendar": "TARGET",
                   "added_holidays": ["2024-07-17", "2025-01-17", "2026-01-19"]}]
BOOTSTRAP_OVERRIDES = [{"calendar": "TARGET",
                        "added_holidays": ["2025-01-16", "2025-01-17"]}]

# On TARGET: 2024-06-14 is a plain Friday, 2024-05-01 is Labour Day (Wednesday),
# 2024-06-15 is a Saturday.
PLAIN_FRIDAY = "2024-06-14"
LABOUR_DAY = "2024-05-01"
SATURDAY = "2024-06-15"
TARGET_OVERRIDE = [{"calendar": "TARGET",
                    "added_holidays": [PLAIN_FRIDAY],
                    "removed_holidays": [LABOUR_DAY]}]


def _calendar_names():
    text = _ENUMS_FBS.read_text()
    body = re.search(r"enum\s+Calendar\s*:\s*\w+\s*\{(.*?)\}", text, re.S).group(1)
    return re.findall(r"^\s*(\w+)\s*=", body, re.M)


CALENDARS = _calendar_names()


def _post(client, endpoint, request):
    r = client.session.post(f"{client.base_url}/{endpoint}", json=request)
    return r.status_code, r.text


def _ok(client, endpoint, request):
    status, body = _post(client, endpoint, request)
    assert status == 200, f"{endpoint}: HTTP {status} :: {body[:300]}"
    return json.loads(body)


def _load(data_dir, filename):
    with open(data_dir / filename) as fh:
        return json.load(fh)


def _holidays_req(calendar="TARGET", start="2024-04-29", end="2024-06-21", overrides=None):
    req = {"calendar": calendar, "start_date": start, "end_date": end,
           "include_weekends": False}
    if overrides is not None:
        req["calendar_overrides"] = overrides
    return req


def _with_pricing_overrides(req, overrides):
    out = copy.deepcopy(req)
    out["pricing"]["calendar_overrides"] = overrides
    return out


# ---------------------------------------------------------------------------
# The three calendar endpoints honour added + removed dates
# ---------------------------------------------------------------------------

def test_calendar_holidays_honours_overrides(client):
    base = _ok(client, "calendar-holidays", _holidays_req())["dates"]
    assert LABOUR_DAY in base and PLAIN_FRIDAY not in base

    got = _ok(client, "calendar-holidays", _holidays_req(overrides=TARGET_OVERRIDE))["dates"]
    assert PLAIN_FRIDAY in got and LABOUR_DAY not in got
    assert sorted(set(base) - {LABOUR_DAY} | {PLAIN_FRIDAY}) == sorted(got)


def test_calendar_business_days_honours_overrides(client):
    req = {"calendar": "TARGET", "start_date": "2024-04-29", "end_date": "2024-06-21",
           "include_start": True, "include_end": True}
    base = _ok(client, "calendar-business-days", req)["dates"]
    assert PLAIN_FRIDAY in base and LABOUR_DAY not in base

    got = _ok(client, "calendar-business-days",
              dict(req, calendar_overrides=TARGET_OVERRIDE))["dates"]
    assert LABOUR_DAY in got and PLAIN_FRIDAY not in got
    assert len(got) == len(base)

    assert _ok(client, "calendar-business-days", req)["dates"] == base


def test_calendar_advance_honours_overrides(client):
    def advance(date, overrides=None):
        req = {"calendar": "TARGET", "date": date, "tenor_number": 1,
               "tenor_unit": "Days", "convention": "Following", "end_of_month": False}
        if overrides is not None:
            req["calendar_overrides"] = overrides
        return _ok(client, "calendar-advance", req)["advanced_date"]

    # Added holiday: Thursday + 1 business day skips the new Friday holiday.
    assert advance("2024-06-13") == PLAIN_FRIDAY
    assert advance("2024-06-13", TARGET_OVERRIDE) == "2024-06-17"
    # Removed holiday: Labour Day becomes the next business day.
    assert advance("2024-04-30") == "2024-05-02"
    assert advance("2024-04-30", TARGET_OVERRIDE) == LABOUR_DAY
    # And nothing lingers.
    assert advance("2024-06-13") == PLAIN_FRIDAY
    assert advance("2024-04-30") == "2024-05-02"


def test_alias_calendars_share_overrides(client):
    override = [{"calendar": "UnitedStates", "added_holidays": [PLAIN_FRIDAY]}]
    got = _ok(client, "calendar-holidays",
              _holidays_req("UnitedStatesSettlement", overrides=override))["dates"]
    assert PLAIN_FRIDAY in got
    # A different United States market is a different calendar.
    other = _ok(client, "calendar-holidays",
                _holidays_req("UnitedStatesNYSE", overrides=override))["dates"]
    assert PLAIN_FRIDAY not in other


def test_accepted_noops_change_nothing(client):
    base = _ok(client, "calendar-holidays", _holidays_req())
    overrides = [
        # already a holiday, a weekend, already a business day
        {"calendar": "TARGET", "added_holidays": [LABOUR_DAY, SATURDAY],
         "removed_holidays": [PLAIN_FRIDAY]},
        # entry without dates; calendar the request does not use
        {"calendar": "Japan"},
        {"calendar": "UnitedKingdom", "added_holidays": [PLAIN_FRIDAY]},
    ]
    assert _ok(client, "calendar-holidays", _holidays_req(overrides=overrides)) == base
    assert _ok(client, "calendar-holidays", _holidays_req(overrides=[])) == base


# ---------------------------------------------------------------------------
# Exhaustive: every Calendar value is overridable-then-clean, or rejected
# ---------------------------------------------------------------------------

def test_calendar_enum_is_fully_listed():
    assert len(CALENDARS) >= 43
    assert INERT_CALENDARS <= set(CALENDARS)


@pytest.mark.parametrize("calendar", CALENDARS)
def test_every_calendar_overridable_or_rejected(client, calendar):
    window = {"start": "2024-06-10", "end": "2024-06-21"}
    business = _ok(client, "calendar-business-days",
                   {"calendar": calendar, "start_date": window["start"],
                    "end_date": window["end"], "include_start": True,
                    "include_end": True})["dates"]
    assert business, f"{calendar}: no business day in the probe window"
    probe = business[0]
    baseline = _ok(client, "calendar-holidays", _holidays_req(calendar, **window))
    assert probe not in baseline["dates"]

    overrides = [{"calendar": calendar, "added_holidays": [probe]}]
    status, body = _post(client, "calendar-holidays",
                         _holidays_req(calendar, overrides=overrides, **window))
    if calendar in INERT_CALENDARS:
        assert status == 400, f"{calendar}: expected 400, got {status} :: {body[:200]}"
        assert "calendar_overrides[0].calendar" in body
    else:
        assert status == 200, f"{calendar}: HTTP {status} :: {body[:200]}"
        assert probe in json.loads(body)["dates"], f"{calendar}: override not visible"

    # Either way the next plain request sees the untouched calendar.
    assert _ok(client, "calendar-holidays", _holidays_req(calendar, **window)) == baseline


# ---------------------------------------------------------------------------
# Leak tests: with overrides, then without -> equals the pre-override baseline
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("endpoint,filename,overrides", [
    ("price-vanilla-swap", SWAP_FILE, SWAP_OVERRIDES),
    ("bootstrap-curves", BOOTSTRAP_FILE, BOOTSTRAP_OVERRIDES),
])
def test_pricing_overrides_do_not_leak(client, data_dir, endpoint, filename, overrides):
    req = _load(data_dir, filename)
    baseline = _ok(client, endpoint, req)
    overridden = _ok(client, endpoint, _with_pricing_overrides(req, overrides))
    assert overridden != baseline, "override did not change the result; test proves nothing"
    assert _ok(client, endpoint, req) == baseline
    # Overrides sent to a pricing endpoint do not reach the calendar endpoints.
    assert overrides[0]["added_holidays"][0] not in _ok(
        client, "calendar-holidays",
        _holidays_req(start="2024-01-01", end="2025-12-31"))["dates"]


def test_calendar_overrides_do_not_leak(client):
    baseline = _ok(client, "calendar-holidays", _holidays_req())
    overridden = _ok(client, "calendar-holidays", _holidays_req(overrides=TARGET_OVERRIDE))
    assert overridden != baseline
    assert _ok(client, "calendar-holidays", _holidays_req()) == baseline


def test_overrides_do_not_leak_when_calendar_request_fails(client):
    baseline = _ok(client, "calendar-holidays", _holidays_req())
    status, body = _post(client, "calendar-holidays",
                         _holidays_req(start="not-a-date", overrides=TARGET_OVERRIDE))
    assert 400 <= status < 500, f"expected 4xx, got {status} :: {body[:200]}"
    assert "calendar_overrides" not in body
    assert _ok(client, "calendar-holidays", _holidays_req()) == baseline


def test_overrides_do_not_leak_when_pricing_request_fails(client, data_dir):
    req = _load(data_dir, SWAP_FILE)
    baseline = _ok(client, "price-vanilla-swap", req)
    calendar_baseline = _ok(client, "calendar-holidays",
                            _holidays_req(start="2024-01-01", end="2024-12-31"))

    failing = _with_pricing_overrides(req, SWAP_OVERRIDES)
    failing["swaps"][0]["discounting_curve"] = "NO_SUCH_CURVE"
    status, body = _post(client, "price-vanilla-swap", failing)
    assert 400 <= status < 500, f"expected 4xx, got {status} :: {body[:200]}"
    assert "calendar_overrides" not in body

    assert _ok(client, "price-vanilla-swap", req) == baseline
    assert _ok(client, "calendar-holidays",
               _holidays_req(start="2024-01-01", end="2024-12-31")) == calendar_baseline


def test_added_holiday_moving_fixing_before_as_of_needs_that_fixing(client, data_dir):
    # as_of 2024-01-15, first floating period starts 2024-01-17, 2 fixing days:
    # the first Euribor6M fixing falls on the as-of date, so it is forecast.
    # Making 2024-01-16/17 TARGET holidays pushes that fixing date back to
    # 2024-01-12, which is in the past and not supplied -> the usual
    # missing-fixing error.
    req = _load(data_dir, SWAP_FILE)
    baseline = _ok(client, "price-vanilla-swap", req)

    failing = _with_pricing_overrides(
        req, [{"calendar": "TARGET", "added_holidays": ["2024-01-16", "2024-01-17"]}])
    status, body = _post(client, "price-vanilla-swap", failing)
    assert status == 422, f"expected 422, got {status} :: {body[:300]}"
    message = json.loads(body)["message"]
    assert "Missing Euribor6M" in message, message
    assert "fixing for January 12th, 2024" in message, message

    # The failed request leaves no overrides behind.
    assert _ok(client, "price-vanilla-swap", req) == baseline


# ---------------------------------------------------------------------------
# Error contract: every rejection is a 400 naming the field path
# ---------------------------------------------------------------------------

REJECTIONS = [
    ("entry without calendar",
     [{"calendar": "TARGET"}, {"added_holidays": [PLAIN_FRIDAY]}],
     "calendar_overrides[1].calendar"),
    ("unparseable date",
     [{"calendar": "TARGET", "added_holidays": [PLAIN_FRIDAY, "14/06/2024"]}],
     "calendar_overrides[0].added_holidays[1]"),
    ("impossible date",
     [{"calendar": "TARGET", "removed_holidays": ["2024-02-30"]}],
     "calendar_overrides[0].removed_holidays[0]"),
    ("out-of-range date",
     [{"calendar": "TARGET", "added_holidays": ["1800-01-01"]}],
     "calendar_overrides[0].added_holidays[0]"),
    ("same date in both lists",
     [{"calendar": "TARGET", "added_holidays": [PLAIN_FRIDAY],
       "removed_holidays": [PLAIN_FRIDAY]}],
     "calendar_overrides[0].removed_holidays[0]"),
    ("duplicate date in added_holidays",
     [{"calendar": "TARGET", "added_holidays": [PLAIN_FRIDAY, PLAIN_FRIDAY]}],
     "calendar_overrides[0].added_holidays[1]"),
    ("duplicate date in removed_holidays",
     [{"calendar": "TARGET", "removed_holidays": [LABOUR_DAY, LABOUR_DAY]}],
     "calendar_overrides[0].removed_holidays[1]"),
    ("same calendar in two entries",
     [{"calendar": "TARGET", "added_holidays": [PLAIN_FRIDAY]},
      {"calendar": "TARGET", "removed_holidays": [LABOUR_DAY]}],
     "calendar_overrides[1].calendar"),
    ("aliased calendar in two entries",
     [{"calendar": "UnitedStates", "added_holidays": [PLAIN_FRIDAY]},
      {"calendar": "UnitedStatesSettlement", "added_holidays": ["2024-06-13"]}],
     "calendar_overrides[1].calendar"),
    ("NullCalendar cannot be overridden",
     [{"calendar": "NullCalendar", "added_holidays": [PLAIN_FRIDAY]}],
     "calendar_overrides[0].calendar"),
    ("BespokeCalendar cannot be overridden",
     [{"calendar": "BespokeCalendar", "added_holidays": [PLAIN_FRIDAY]}],
     "calendar_overrides[0].calendar"),
    ("weekend date in removed_holidays",
     [{"calendar": "TARGET", "removed_holidays": [SATURDAY]}],
     "calendar_overrides[0].removed_holidays[0]"),
]


@pytest.mark.parametrize("label,overrides,path", REJECTIONS, ids=[r[0] for r in REJECTIONS])
def test_rejections_on_calendar_endpoints(client, label, overrides, path):
    requests = {
        "calendar-holidays": _holidays_req(overrides=overrides),
        "calendar-business-days": {"calendar": "TARGET", "start_date": "2024-06-10",
                                   "end_date": "2024-06-21", "include_start": True,
                                   "include_end": True, "calendar_overrides": overrides},
        "calendar-advance": {"calendar": "TARGET", "date": "2024-06-13", "tenor_number": 1,
                             "tenor_unit": "Days", "convention": "Following",
                             "end_of_month": False, "calendar_overrides": overrides},
    }
    for endpoint, req in requests.items():
        status, body = _post(client, endpoint, req)
        assert status == 400, f"{label} @ {endpoint}: HTTP {status} :: {body[:200]}"
        assert path in body, f"{label} @ {endpoint}: field path missing :: {body[:300]}"
    # A rejected request applies nothing.
    assert PLAIN_FRIDAY not in _ok(client, "calendar-holidays", _holidays_req())["dates"]


@pytest.mark.parametrize("label,overrides,path", REJECTIONS, ids=[r[0] for r in REJECTIONS])
@pytest.mark.parametrize("endpoint,filename", [
    ("price-vanilla-swap", SWAP_FILE),
    ("bootstrap-curves", BOOTSTRAP_FILE),
])
def test_rejections_on_pricing_endpoints(client, data_dir, endpoint, filename,
                                         label, overrides, path):
    req = _with_pricing_overrides(_load(data_dir, filename), overrides)
    status, body = _post(client, endpoint, req)
    assert status == 400, f"{label} @ {endpoint}: HTTP {status} :: {body[:200]}"
    assert "pricing." + path in body, f"{label} @ {endpoint}: field path missing :: {body[:300]}"
