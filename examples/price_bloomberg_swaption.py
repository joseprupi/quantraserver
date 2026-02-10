#!/usr/bin/env python3
"""
Price the Bloomberg SOFR 1Mx10Y swaption via JSON API.
"""

import json
import requests
from pathlib import Path


API_URL = "http://localhost:8080/price-swaption"
REQUEST_PATH = Path(__file__).resolve().parent / "data" / "swaption_ois_bbg_zerorate_request.json"
TARGET_NPV = 10359.49


def main():
    if not REQUEST_PATH.exists():
        raise SystemExit(f"Missing request file: {REQUEST_PATH}")

    payload = json.loads(REQUEST_PATH.read_text())
    r = requests.post(API_URL, json=payload, timeout=30)
    r.raise_for_status()
    data = r.json()

    npv = data["swaptions"][0]["npv"]
    diff = npv - TARGET_NPV

    print(f"NPV: {npv:.6f}")
    print(f"Target (Bloomberg): {TARGET_NPV:.6f}")
    print(f"Diff: {diff:.6f}")


if __name__ == "__main__":
    main()
