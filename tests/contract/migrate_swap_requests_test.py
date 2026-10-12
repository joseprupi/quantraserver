"""Unit tests for tools/migrate_swap_requests (0.9.0 -> 0.10.0 SwapLeg shape).

Pure Python: no server, no QuantLib. Every vanilla / OIS / basis swap and
swaption request under examples/data must convert without error, pass the
structural 0.10.0 check (plan/18 §2b) and convert to itself a second time.
The `--check` mode is exercised against a fake HTTP server.
"""

from __future__ import annotations

import copy
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TOOL_DIR = REPO / "tools" / "migrate_swap_requests"
DATA_DIR = REPO / "examples" / "data"
sys.path.insert(0, str(TOOL_DIR))

import migrate_swap_requests as msr  # noqa: E402

OLD_ONLY_KEYS = {"swap_type", "cms_leg", "forwarding_curve_leg1", "forwarding_curve_leg2"}


def _load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def _corpus() -> list[Path]:
    files = []
    for f in sorted(DATA_DIR.rglob("*.json")):
        try:
            msr.detect_endpoint(_load(f))
        except (msr.NotASwapRequest, msr.ConversionError, json.JSONDecodeError):
            continue
        files.append(f)
    return files


CORPUS = _corpus()


def _walk_keys(node, path=""):
    if isinstance(node, dict):
        for k, v in node.items():
            yield f"{path}.{k}" if path else k
            yield from _walk_keys(v, f"{path}.{k}" if path else k)
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from _walk_keys(v, f"{path}[{i}]")


def _fixture(name: str) -> dict:
    return _load(DATA_DIR / name)


# --------------------------------------------------------------------------- #
# Corpus
# --------------------------------------------------------------------------- #

def test_corpus_is_what_we_expect():
    counts = {}
    for f in CORPUS:
        counts[msr.detect_endpoint(_load(f))] = counts.get(msr.detect_endpoint(_load(f)), 0) + 1
    assert counts["vanilla_swap"] >= 30 and counts["ois_swap"] >= 5
    assert counts["basis_swap"] >= 3 and counts["swaption"] >= 15


@pytest.mark.parametrize("path", CORPUS, ids=[str(p.relative_to(DATA_DIR)) for p in CORPUS])
def test_corpus_fixture_converts_validates_and_is_idempotent(path):
    original = _load(path)
    snapshot = copy.deepcopy(original)
    converted = msr.convert_request(original)
    assert original == snapshot, "convert_request must not mutate its input"
    assert msr.validate_converted(converted) == []
    assert msr.convert_request(converted) == converted, "not idempotent"
    # Non-swap content verbatim, old-only keys gone, no wrapper curves.
    assert converted.get("pricing") == original.get("pricing")
    for k in ("include_flows", "include_diagnostics"):
        assert converted.get(k) == original.get(k)
    keys = list(_walk_keys(converted))
    for k in keys:
        leaf = k.rsplit(".", 1)[-1]
        assert leaf not in OLD_ONLY_KEYS, k
    list_key = "swaptions" if "swaptions" in converted else "swaps"
    for i, item in enumerate(converted[list_key]):
        assert "discounting_curve" not in item and "forwarding_curve" not in item, i
    # Top-level key order preserved.
    assert list(converted.keys()) == list(original.keys())


# --------------------------------------------------------------------------- #
# Mapping rules
# --------------------------------------------------------------------------- #

def _legs(converted: dict, table: str) -> dict:
    return converted["swaps"][0][table]


def test_payer_maps_fixed_leg_to_pay():
    req = _fixture("ir_swaps/irs_eur_5y_payer_fixed_30360_vs_euribor6m.json")
    legs = _legs(msr.convert_request(req), "vanilla_swap")
    assert list(legs.keys()) == ["fixed_leg", "floating_leg"]
    assert legs["fixed_leg"]["pay_receive"] == "Pay"
    assert legs["floating_leg"]["pay_receive"] == "Receive"
    assert list(legs["fixed_leg"].keys()) == [
        "pay_receive", "currency", "coupons_type", "coupons", "discounting_curve"]
    assert list(legs["floating_leg"].keys()) == [
        "pay_receive", "currency", "coupons_type", "coupons", "discounting_curve",
        "forwarding_curve"]


def test_receiver_maps_fixed_leg_to_receive():
    req = _fixture("ir_swaps/irs_eur_5y_receiver_fixed_30360_vs_euribor6m.json")
    legs = _legs(msr.convert_request(req), "vanilla_swap")
    assert legs["fixed_leg"]["pay_receive"] == "Receive"
    assert legs["floating_leg"]["pay_receive"] == "Pay"


def test_vanilla_curves_and_currency_and_coupons_verbatim():
    req = _fixture("ir_swaps/irs_eur_5y_payer_ois_discounted_multicurve.json")
    item = req["swaps"][0]
    legs = _legs(msr.convert_request(req), "vanilla_swap")
    assert legs["fixed_leg"]["discounting_curve"] == item["discounting_curve"]
    assert legs["floating_leg"]["discounting_curve"] == item["discounting_curve"]
    assert legs["floating_leg"]["forwarding_curve"] == item["forwarding_curve"]
    assert "forwarding_curve" not in legs["fixed_leg"]
    assert legs["fixed_leg"]["coupons"] == item["vanilla_swap"]["fixed_leg"]
    assert legs["floating_leg"]["coupons"] == item["vanilla_swap"]["floating_leg"]
    assert legs["fixed_leg"]["coupons_type"] == "FixedCoupons"
    assert legs["floating_leg"]["coupons_type"] == "IborCoupons"
    index_id = item["vanilla_swap"]["floating_leg"]["index"]["id"]
    ccy = next(i["currency"] for i in req["pricing"]["rates"]["indices"] if i["id"] == index_id)
    assert legs["fixed_leg"]["currency"] == ccy == legs["floating_leg"]["currency"]


def test_amortizing_and_in_arrears_bodies_are_kept():
    req = _fixture("ir_swaps/irs_eur_5y_amortizing_payer.json")
    legs = _legs(msr.convert_request(req), "vanilla_swap")
    assert legs["fixed_leg"]["coupons"]["notionals"] == \
        req["swaps"][0]["vanilla_swap"]["fixed_leg"]["notionals"]
    req = _fixture("ir_swaps/irs_eur_5y_payer_euribor6m_in_arrears.json")
    legs = _legs(msr.convert_request(req), "vanilla_swap")
    assert legs["floating_leg"]["coupons"]["in_arrears"] is True


def test_ois_fixed_leg_copies_payment_lag_and_calendar():
    req = _fixture("ir_swaps/ois_eur_5y_payer_estr_payment_lag2.json")
    legs = _legs(msr.convert_request(req), "ois_swap")
    assert list(legs.keys()) == ["fixed_leg", "overnight_leg"]
    fixed = legs["fixed_leg"]["coupons"]
    assert fixed["payment_lag"] == 2 and fixed["payment_calendar"] == "TARGET"
    old_fixed = req["swaps"][0]["ois_swap"]["fixed_leg"]
    assert {k: v for k, v in fixed.items() if k not in ("payment_lag", "payment_calendar")} \
        == old_fixed
    assert legs["overnight_leg"]["coupons_type"] == "OvernightCoupons"
    assert legs["overnight_leg"]["coupons"] == req["swaps"][0]["ois_swap"]["overnight_leg"]
    assert legs["overnight_leg"]["forwarding_curve"] == req["swaps"][0]["forwarding_curve"]
    assert "forwarding_curve" not in legs["fixed_leg"]
    assert legs["fixed_leg"]["currency"] == legs["overnight_leg"]["currency"] == "EUR"

    req0 = _fixture("ir_swaps/ois_eur_5y_payer_estr.json")
    fixed0 = _legs(msr.convert_request(req0), "ois_swap")["fixed_leg"]["coupons"]
    assert fixed0["payment_lag"] == req0["swaps"][0]["ois_swap"]["overnight_leg"]["payment_lag"]


def test_ois_receiver_side():
    req = _fixture("ir_swaps/ois_eur_2y_receiver_estr_spread10bp_quarterly.json")
    legs = _legs(msr.convert_request(req), "ois_swap")
    assert legs["fixed_leg"]["pay_receive"] == "Receive"
    assert legs["overnight_leg"]["pay_receive"] == "Pay"


def test_cms_leg_becomes_floating_leg_with_cms_coupons():
    req = _fixture("ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json")
    legs = _legs(msr.convert_request(req), "vanilla_swap")
    assert list(legs.keys()) == ["fixed_leg", "floating_leg"]
    assert legs["floating_leg"]["coupons_type"] == "CmsCoupons"
    assert legs["floating_leg"]["coupons"] == req["swaps"][0]["vanilla_swap"]["cms_leg"]
    assert legs["floating_leg"]["forwarding_curve"] == req["swaps"][0]["forwarding_curve"]
    assert legs["floating_leg"]["currency"] == "EUR"   # via EUR_SWAP_6M -> EUR_6M
    assert legs["floating_leg"]["pay_receive"] == "Receive"


def test_basis_forwarding_curves_per_leg():
    req = _fixture("ir_swaps/basis_eur_5y_3m_vs_6m_ois_discounted_multicurve.json")
    item = req["swaps"][0]
    legs = _legs(msr.convert_request(req), "basis_swap")
    assert list(legs.keys()) == ["leg1", "leg2"]
    assert legs["leg1"]["forwarding_curve"] == item["forwarding_curve_leg1"]
    assert legs["leg2"]["forwarding_curve"] == item["forwarding_curve_leg2"]
    assert legs["leg1"]["discounting_curve"] == legs["leg2"]["discounting_curve"] \
        == item["discounting_curve"]
    assert legs["leg1"]["coupons_type"] == legs["leg2"]["coupons_type"] == "IborCoupons"
    assert legs["leg1"]["pay_receive"] == "Pay" and legs["leg2"]["pay_receive"] == "Receive"
    assert set(msr.convert_request(req)["swaps"][0].keys()) == {"basis_swap"}


def test_swaption_underlying_takes_the_wrapper_curves():
    req = _fixture("swaption_request.json")
    item = req["swaptions"][0]
    out = msr.convert_request(req)["swaptions"][0]
    assert list(out.keys()) == ["swaption", "volatility", "model"]
    assert out["volatility"] == item["volatility"] and out["model"] == item["model"]
    assert list(out["swaption"].keys()) == list(item["swaption"].keys())
    for k in item["swaption"]:
        if k != "underlying":
            assert out["swaption"][k] == item["swaption"][k]
    legs = out["swaption"]["underlying"]
    assert legs["fixed_leg"]["pay_receive"] == "Pay"
    assert legs["fixed_leg"]["discounting_curve"] == item["discounting_curve"]
    assert legs["floating_leg"]["forwarding_curve"] == item["forwarding_curve"]
    assert legs["floating_leg"]["coupons_type"] == "IborCoupons"


def test_swaption_on_ois_underlying():
    req = _fixture("swaption_ois_request.json")
    item = req["swaptions"][0]
    out = msr.convert_request(req)["swaptions"][0]
    legs = out["swaption"]["underlying"]
    assert out["swaption"]["underlying_type"] == "OisSwap"
    assert legs["overnight_leg"]["coupons_type"] == "OvernightCoupons"
    assert legs["overnight_leg"]["forwarding_curve"] == item["forwarding_curve"]
    assert legs["fixed_leg"]["coupons"]["payment_lag"] == \
        item["swaption"]["underlying"]["overnight_leg"]["payment_lag"]
    assert legs["fixed_leg"]["currency"] == "USD"


def test_bermudan_swaption_keeps_exercise_dates():
    req = _fixture("swaption_bermudan_hw_calibrated.json")
    out = msr.convert_request(req)["swaptions"][0]["swaption"]
    assert out["exercise_dates"] == req["swaptions"][0]["swaption"]["exercise_dates"]


def test_detect_endpoint_old_and_new_shapes():
    for name, expected in (("vanilla_swap_request.json", "vanilla_swap"),
                           ("ois_swap_request.json", "ois_swap"),
                           ("basis_swap_request.json", "basis_swap"),
                           ("swaption_request.json", "swaption")):
        req = _fixture(name)
        assert msr.detect_endpoint(req) == expected
        assert msr.detect_endpoint(msr.convert_request(req)) == expected


# --------------------------------------------------------------------------- #
# Error cases
# --------------------------------------------------------------------------- #

def _err(req: dict) -> msr.ConversionError:
    with pytest.raises(msr.ConversionError) as info:
        msr.convert_request(req)
    return info.value


def test_missing_index_currency_is_an_error_naming_the_index():
    req = _fixture("vanilla_swap_request.json")
    idx = req["swaps"][0]["vanilla_swap"]["floating_leg"]["index"]["id"]
    for d in req["pricing"]["rates"]["indices"]:
        d.pop("currency", None)
    err = _err(req)
    assert err.field == f"pricing.rates.indices[{idx}].currency"


def test_missing_swap_index_currency_path_for_cms():
    req = _fixture("ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json")
    for d in req["pricing"]["rates"]["indices"]:
        d.pop("currency", None)
    err = _err(req)
    assert err.field == "pricing.rates.indices[EUR_6M].currency"
    req = _fixture("ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json")
    req["swaps"][0]["vanilla_swap"]["cms_leg"]["swap_index_id"] = "NOPE"
    assert _err(req).field == "swaps[0].vanilla_swap.cms_leg.swap_index_id"


def test_unknown_index_is_an_error():
    req = _fixture("vanilla_swap_request.json")
    req["swaps"][0]["vanilla_swap"]["floating_leg"]["index"]["id"] = "NO_SUCH"
    assert _err(req).field == "swaps[0].vanilla_swap.floating_leg.index.id"


def test_other_swap_kinds_are_not_swap_requests():
    for name in ("zero_coupon_swap_request.json", "fixed_rate_bond_request.json",
                 "cross_currency_swap/xccy_eur_fixed_at_fair_rate_vs_usd_sofr_5y.json"):
        with pytest.raises(msr.NotASwapRequest):
            msr.detect_endpoint(_fixture(name))
        with pytest.raises(msr.NotASwapRequest):
            msr.convert_request(_fixture(name))
    with pytest.raises(msr.NotASwapRequest):
        msr.detect_endpoint({"swaps": []})
    with pytest.raises(msr.NotASwapRequest):
        msr.detect_endpoint([])


def test_mixed_swap_kinds_in_one_list_is_an_error():
    a = _fixture("vanilla_swap_request.json")
    b = _fixture("ois_swap_request.json")
    a["swaps"].append(b["swaps"][0])
    with pytest.raises(msr.ConversionError):
        msr.detect_endpoint(a)


def test_leftover_unknown_field_on_wrapper_is_an_error():
    req = _fixture("vanilla_swap_request.json")
    req["swaps"][0]["extra_thing"] = 1
    assert _err(req).field == "swaps[0].extra_thing"


def test_unknown_field_on_swap_table_is_an_error():
    req = _fixture("ois_swap_request.json")
    req["swaps"][0]["ois_swap"]["spread_leg"] = {}
    assert _err(req).field == "swaps[0].ois_swap.spread_leg"


def test_missing_or_invalid_swap_type():
    req = _fixture("basis_swap_request.json")
    del req["swaps"][0]["basis_swap"]["swap_type"]
    assert _err(req).field == "swaps[0].basis_swap.swap_type"
    req = _fixture("basis_swap_request.json")
    req["swaps"][0]["basis_swap"]["swap_type"] = "Long"
    assert _err(req).field == "swaps[0].basis_swap.swap_type"


def test_missing_wrapper_curve_is_an_error():
    req = _fixture("vanilla_swap_request.json")
    del req["swaps"][0]["forwarding_curve"]
    assert _err(req).field == "swaps[0].forwarding_curve"
    req = _fixture("swaption_request.json")
    del req["swaptions"][0]["discounting_curve"]
    assert _err(req).field == "swaptions[0].discounting_curve"


def test_both_floating_and_cms_leg_is_an_error():
    req = _fixture("ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json")
    req["swaps"][0]["vanilla_swap"]["floating_leg"] = \
        _fixture("vanilla_swap_request.json")["swaps"][0]["vanilla_swap"]["floating_leg"]
    assert _err(req).field == "swaps[0].vanilla_swap.cms_leg"
    req = _fixture("vanilla_swap_request.json")
    del req["swaps"][0]["vanilla_swap"]["floating_leg"]
    assert _err(req).field == "swaps[0].vanilla_swap.floating_leg"


def test_mixed_old_and_new_shape_is_an_error():
    new = msr.convert_request(_fixture("vanilla_swap_request.json"))
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["swap_type"] = "Payer"
    assert _err(bad).field == "swaps[0].vanilla_swap.swap_type"
    bad = copy.deepcopy(new)
    bad["swaps"][0]["discounting_curve"] = "discount"
    assert _err(bad).field == "swaps[0].discounting_curve"
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["cms_leg"] = {}
    assert _err(bad).field == "swaps[0].vanilla_swap.cms_leg"
    new_sw = msr.convert_request(_fixture("swaption_request.json"))
    bad = copy.deepcopy(new_sw)
    bad["swaptions"][0]["forwarding_curve"] = "discount"
    assert _err(bad).field == "swaptions[0].forwarding_curve"


def test_ois_fixed_leg_with_its_own_lag_in_the_old_shape_is_an_error():
    req = _fixture("ois_swap_request.json")
    req["swaps"][0]["ois_swap"]["fixed_leg"]["payment_lag"] = 1
    assert _err(req).field == "swaps[0].ois_swap.fixed_leg.payment_lag"


def test_bad_swaption_underlying_type():
    req = _fixture("swaption_request.json")
    req["swaptions"][0]["swaption"]["underlying_type"] = "BasisSwap"
    assert _err(req).field == "swaptions[0].swaption.underlying_type"


def test_basis_legs_in_different_currencies_is_an_error():
    req = _fixture("basis_swap_request.json")
    leg2_idx = req["swaps"][0]["basis_swap"]["leg2"]["index"]["id"]
    for d in req["pricing"]["rates"]["indices"]:
        if d["id"] == leg2_idx:
            d["currency"] = "USD"
    assert _err(req).field == "swaps[0].basis_swap"


# --------------------------------------------------------------------------- #
# Structural validator
# --------------------------------------------------------------------------- #

def test_validator_flags_old_shape_and_bad_unions():
    old = _fixture("vanilla_swap_request.json")
    assert msr.validate_converted(old)
    new = msr.convert_request(old)
    assert msr.validate_converted(new) == []
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["floating_leg"]["coupons_type"] = "OvernightCoupons"
    assert any("coupons_type" in p for p in msr.validate_converted(bad))
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["floating_leg"]["pay_receive"] = "Pay"
    assert any("both legs are Pay" in p for p in msr.validate_converted(bad))
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["fixed_leg"]["forwarding_curve"] = "x"
    assert any("forwarding_curve: present on a fixed leg" in p for p in msr.validate_converted(bad))
    bad = copy.deepcopy(new)
    del bad["swaps"][0]["vanilla_swap"]["floating_leg"]["forwarding_curve"]
    assert any("forwarding_curve: missing" in p for p in msr.validate_converted(bad))
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["fixed_leg"]["currency"] = "eur"
    assert any("currency" in p for p in msr.validate_converted(bad))
    ois = msr.convert_request(_fixture("ois_swap_request.json"))
    bad = copy.deepcopy(ois)
    bad["swaps"][0]["ois_swap"]["overnight_leg"]["coupons_type"] = "IborCoupons"
    assert any("coupons_type" in p for p in msr.validate_converted(bad))


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def test_cli_out_dir_writes_converted_files_and_skips_others(tmp_path, capsys):
    src = tmp_path / "in"
    (src / "sub").mkdir(parents=True)
    (src / "sub" / "v.json").write_text(json.dumps(_fixture("vanilla_swap_request.json")))
    (src / "bond.json").write_text(json.dumps(_fixture("fixed_rate_bond_request.json")))
    out = tmp_path / "out"
    rc = msr.main([str(src), "--out", str(out)])
    text = capsys.readouterr().out
    assert rc == 0
    assert (out / "sub" / "v.json").exists() and not (out / "bond.json").exists()
    written = json.loads((out / "sub" / "v.json").read_text())
    assert msr.validate_converted(written) == []
    assert (out / "sub" / "v.json").read_text().endswith("}\n")
    assert "converted 1 (vanilla_swap=1" in text and "skipped 1" in text and "errors 0" in text


def test_cli_in_place_and_idempotent_rerun(tmp_path, capsys):
    f = tmp_path / "o.json"
    f.write_text(json.dumps(_fixture("ois_swap_request.json"), indent=2) + "\n")
    assert msr.main([str(f), "--in-place"]) == 0
    first = f.read_text()
    assert "swap_type" not in first and '"coupons_type": "OvernightCoupons"' in first
    assert msr.main([str(f), "--in-place"]) == 0
    assert f.read_text() == first
    assert "1 already in the 0.10.0 shape" in capsys.readouterr().out


def test_cli_error_names_file_and_field(tmp_path, capsys):
    req = _fixture("vanilla_swap_request.json")
    req["swaps"][0]["leftover"] = True
    f = tmp_path / "bad.json"
    f.write_text(json.dumps(req))
    good = tmp_path / "good.json"
    good.write_text(json.dumps(_fixture("basis_swap_request.json")))
    rc = msr.main([str(f), str(good), "--out", str(tmp_path / "out")])
    text = capsys.readouterr().out
    assert rc == 1
    assert f"ERROR {f}: swaps[0].leftover" in text
    assert (tmp_path / "out" / "good.json").exists()
    assert "errors 1" in text


def test_cli_invalid_json_and_missing_input(tmp_path, capsys):
    f = tmp_path / "x.json"
    f.write_text("{not json")
    assert msr.main([str(f)]) == 1
    assert "not valid JSON" in capsys.readouterr().out
    assert msr.main([str(tmp_path / "nope.json")]) == 2


# --------------------------------------------------------------------------- #
# --check against a fake server
# --------------------------------------------------------------------------- #

class _Fake:
    """Minimal HTTP server returning canned JSON per path; records bodies."""

    def __init__(self, responses: dict):
        self.responses = responses
        self.requests: list[tuple[str, dict]] = []
        fake = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(n))
                fake.requests.append((self.path, body))
                resp = fake.responses.get(self.path)
                if resp is None:
                    self.send_response(404)
                    self.end_headers()
                    self.wfile.write(b"no route")
                    return
                data = json.dumps(resp).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *a):
                pass

        self.server = HTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def close(self):
        self.server.shutdown()
        self.server.server_close()


@pytest.fixture
def fake_servers():
    made = []

    def make(responses):
        s = _Fake(responses)
        made.append(s)
        return s

    yield make
    for s in made:
        s.close()


def test_check_matching_responses_pass(tmp_path, fake_servers, capsys):
    old = fake_servers({"/price-vanilla-swap": {"swaps": [{
        "npv": 1234.5, "fair_rate": 0.0312, "fair_spread": 0.0011,
        "fixed_leg_npv": -5000.25, "floating_leg_npv": 6234.75}]}})
    new = fake_servers({"/price-vanilla-swap": {"swaps": [{
        "npv": 1234.5 + 1e-6, "fair_rate": 0.0312, "fair_spread": 0.0011,
        "legs": [{"npv": -5000.25}, {"npv": 6234.75}]}]}})
    f = tmp_path / "v.json"
    f.write_text(json.dumps(_fixture("vanilla_swap_request.json")))
    rc = msr.main([str(f), "--check", old.url, new.url])
    text = capsys.readouterr().out
    assert rc == 0, text
    assert "OK" in text and "BAD" not in text
    assert old.requests[0][0] == "/price-vanilla-swap" and "swap_type" in json.dumps(old.requests[0][1])
    assert "swap_type" not in json.dumps(new.requests[0][1])
    assert msr.validate_converted(new.requests[0][1]) == []


def test_check_mismatch_fails_and_prints_table(tmp_path, fake_servers, capsys):
    old = fake_servers({"/price-ois-swap": {"swaps": [{
        "npv": 100.0, "fair_rate": 0.03, "fair_spread": 0.0,
        "fixed_leg_npv": -10.0, "overnight_leg_npv": 110.0}]}})
    new = fake_servers({"/price-ois-swap": {"swaps": [{
        "npv": 100.5, "fair_rate": 0.03, "fair_spread": 0.0,
        "legs": [{"npv": -10.0}, {"npv": 110.5}]}]}})
    f = tmp_path / "o.json"
    f.write_text(json.dumps(_fixture("ois_swap_request.json")))
    rc = msr.main([str(f), "--check", old.url, new.url])
    text = capsys.readouterr().out
    assert rc == 1
    assert "FAIL" in text and "BAD swaps[0].npv" in text
    assert "BAD swaps[0].legs[1].npv (old overnight_leg_npv)" in text
    assert "ok  swaps[0].legs[0].npv (old fixed_leg_npv)" in text


def test_check_http_error_and_already_converted(tmp_path, fake_servers, capsys):
    old = fake_servers({})
    new = fake_servers({})
    f = tmp_path / "v.json"
    f.write_text(json.dumps(_fixture("vanilla_swap_request.json")))
    g = tmp_path / "already.json"
    g.write_text(json.dumps(msr.convert_request(_fixture("vanilla_swap_request.json"))))
    rc = msr.main([str(f), str(g), "--check", old.url, new.url])
    text = capsys.readouterr().out
    assert rc == 1
    assert "HTTP 404" in text and "already in the 0.10.0 shape" in text


def test_compare_responses_basis_and_swaption_and_tolerance():
    basis = msr.convert_request(_fixture("ir_swaps/basis_eur_5y_euribor3m_plus5bp_vs_euribor6m.json"))
    spreads = [basis["swaps"][0]["basis_swap"][r]["coupons"].get("spread", 0.0)
               for r in ("leg1", "leg2")]
    assert spreads[0] != 0.0 and spreads[1] == 0.0
    old = {"swaps": [{"npv": 10.0, "leg1_npv": 1.0, "leg2_npv": 9.0,
                      "fair_spread_leg1": 0.0007, "fair_spread_leg2": -0.0006}]}
    new = {"swaps": [{"npv": 10.0, "fair_spread": 0.0007, "legs": [{"npv": 1.0}, {"npv": 9.0}]}]}
    rows = msr.compare_responses("basis_swap", basis, old, new)
    assert all(ok for _, _, _, ok in rows), rows
    assert any("fair_spread (old leg1)" in r[0] for r in rows)
    new["swaps"][0]["fair_spread"] = -0.0006
    rows = msr.compare_responses("basis_swap", basis, old, new)
    assert [r for r in rows if "fair_spread" in r[0]][0][3] is False

    sw = msr.convert_request(_fixture("swaption_request.json"))
    rows = msr.compare_responses("swaption", sw, {"swaptions": [{"npv": 5.0}]},
                                 {"swaptions": [{"npv": 5.0}]})
    assert rows == [("swaptions[0].npv", 5.0, 5.0, True)]

    # Money tolerance scales with the notional (1e6 here -> 1e-3 abs).
    van = msr.convert_request(_fixture("vanilla_swap_request.json"))
    notional = van["swaps"][0]["vanilla_swap"]["fixed_leg"]["coupons"]["notional"]
    base = {"npv": 1.0, "fair_rate": 0.03, "fair_spread": 0.0}
    ok = dict(base, npv=1.0 + 0.5e-9 * notional)
    bad = dict(base, npv=1.0 + 2e-9 * notional)
    assert all(r[3] for r in msr.compare_responses("vanilla_swap", van, {"swaps": [base]}, {"swaps": [ok]}))
    assert not all(r[3] for r in msr.compare_responses("vanilla_swap", van, {"swaps": [base]}, {"swaps": [bad]}))
    # Rates do not scale with the notional.
    bad_rate = dict(base, fair_rate=0.03 + 1e-9)
    assert not all(r[3] for r in msr.compare_responses("vanilla_swap", van, {"swaps": [base]}, {"swaps": [bad_rate]}))
    # A missing field on either side is a mismatch, never a silent pass.
    rows = msr.compare_responses("vanilla_swap", van, {"swaps": [base]}, {"swaps": [{"npv": 1.0}]})
    assert [r[3] for r in rows] == [True, False, False]
