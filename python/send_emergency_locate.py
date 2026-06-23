"""
send_emergency_locate.py
------------------------
Send a signed emergency GPS locate request to POST /emergency/locate.

Examples:
  python python/send_emergency_locate.py --vehicle AMB001 --lat 32.0861 --lon 34.7818
  python python/send_emergency_locate.py --server http://127.0.0.1:8000 --vehicle POL001 --lat 32.0844 --lon 34.7817
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict

ROOT = Path(__file__).resolve().parents[1]
EMERGENCY_KEYS_PATHS = [
    ROOT / "python" / "server" / "emergency_keys.json",
    ROOT / "server" / "emergency_keys.json",
]


def load_emergency_keys() -> Dict[str, str]:
    for path in EMERGENCY_KEYS_PATHS:
        if path.exists():
            payload = json.loads(path.read_text(encoding="utf-8"))
            return payload.get("vehicle_keys", {})
    return {
        "AMB001": "demo-emergency-key-001",
        "POL001": "demo-emergency-key-002",
    }


def sign_locate(vehicle_id: str, latitude: float, longitude: float, timestamp: float, secret: str) -> str:
    payload = f"{vehicle_id}|{latitude:.7f}|{longitude:.7f}|{timestamp:.3f}"
    return hmac.new(secret.encode("utf-8"), payload.encode("utf-8"), hashlib.sha256).hexdigest()


def http_json_post(url: str, body: Dict[str, Any]) -> Dict[str, Any]:
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        url,
        method="POST",
        data=data,
        headers={
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        raw = resp.read().decode("utf-8")
        return json.loads(raw) if raw else {}


def main() -> int:
    parser = argparse.ArgumentParser(description="Send signed emergency locate request")
    parser.add_argument("--server", default="http://127.0.0.1:8000", help="Server base URL")
    parser.add_argument("--vehicle", default="AMB001", help="Emergency vehicle ID")
    parser.add_argument("--lat", type=float, required=True, help="GPS latitude")
    parser.add_argument("--lon", type=float, required=True, help="GPS longitude")
    args = parser.parse_args()

    keys = load_emergency_keys()
    secret = keys.get(args.vehicle) or keys.get("*")
    if not secret:
        print(f"vehicle_id '{args.vehicle}' was not found in emergency keys")
        print(f"available keys: {', '.join(sorted(keys.keys()))}")
        return 1

    ts = time.time()
    signature = sign_locate(args.vehicle, args.lat, args.lon, ts, secret)
    body = {
        "vehicle_id": args.vehicle,
        "latitude": args.lat,
        "longitude": args.lon,
        "timestamp": ts,
        "signature": signature,
    }

    try:
        response = http_json_post(f"{args.server}/emergency/locate", body)
        print("request sent successfully")
        print(json.dumps(response, ensure_ascii=False, indent=2))
        return 0
    except urllib.error.HTTPError as exc:
        details = exc.read().decode("utf-8", errors="ignore")
        print(f"HTTP {exc.code}: {details}")
        return 1
    except Exception as exc:
        print(f"error: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
