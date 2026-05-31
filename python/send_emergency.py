"""
send_emergency.py
-----------------
שולח אות חירום חתום לשרת FastAPI.

דוגמאות:
  python python/send_emergency.py --intersection 1 --lane 0 --vehicle AMB001
  python python/send_emergency.py --intersection 1 --clear
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
from typing import Any, Dict, List

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


def http_json(method: str, url: str, body: Dict[str, Any] | None = None) -> Dict[str, Any]:
    data = None
    headers = {"Accept": "application/json"}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"

    req = urllib.request.Request(url, method=method.upper(), data=data, headers=headers)
    with urllib.request.urlopen(req, timeout=10) as resp:
        raw = resp.read().decode("utf-8")
        return json.loads(raw) if raw else {}


def build_default_lanes(num_lanes: int) -> List[Dict[str, Any]]:
    return [
        {
            "lane_id": i,
            "vehicle_count": 0,
            "density_pct": 0.0,
            "waiting_time_sec": 0.0,
        }
        for i in range(num_lanes)
    ]


def fetch_or_build_state(server_base: str, intersection_id: int, num_lanes: int) -> Dict[str, Any]:
    try:
        state = http_json("GET", f"{server_base}/intersection/{intersection_id}")
        return state
    except urllib.error.HTTPError as e:
        if e.code != 404:
            raise
    except Exception:
        pass

    return {
        "intersection_id": intersection_id,
        "num_lanes": num_lanes,
        "timestamp": time.time(),
        "lanes": build_default_lanes(num_lanes),
        "neighbor_states": {},
    }


def sign_emergency(vehicle_id: str, lane_id: int, timestamp: float, secret: str) -> str:
    payload = f"{vehicle_id}|{lane_id}|{timestamp:.3f}"
    return hmac.new(secret.encode("utf-8"), payload.encode("utf-8"), hashlib.sha256).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description="Send signed emergency signal to Smart Traffic server")
    parser.add_argument("--server", default="http://127.0.0.1:8000", help="Server base URL")
    parser.add_argument("--intersection", type=int, default=1, help="Intersection ID")
    parser.add_argument("--lane", type=int, default=0, help="Lane ID (0..3)")
    parser.add_argument("--vehicle", default="AMB001", help="Emergency vehicle ID")
    parser.add_argument("--num-lanes", type=int, default=4, help="Fallback number of lanes")
    parser.add_argument("--clear", action="store_true", help="Clear emergency (active=false)")
    args = parser.parse_args()

    keys = load_emergency_keys()
    secret = keys.get(args.vehicle)
    if not secret and not args.clear:
        print(f"❌ vehicle_id '{args.vehicle}' לא קיים ב-emergency_keys.json")
        print(f"רכבים זמינים: {', '.join(sorted(keys.keys()))}")
        return 1

    state = fetch_or_build_state(args.server, args.intersection, args.num_lanes)
    state["timestamp"] = time.time()

    if args.clear:
        state["emergency_signal"] = {
            "active": False,
            "lane_id": args.lane,
            "vehicle_id": args.vehicle,
            "timestamp": time.time(),
            "signature": "",
        }
    else:
        ts = time.time()
        signature = sign_emergency(args.vehicle, args.lane, ts, secret)
        state["emergency_signal"] = {
            "active": True,
            "lane_id": args.lane,
            "vehicle_id": args.vehicle,
            "timestamp": ts,
            "signature": signature,
        }

    try:
        result = http_json("POST", f"{args.server}/state", state)
        print("✅ הבקשה נשלחה בהצלחה")
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except urllib.error.HTTPError as e:
        details = e.read().decode("utf-8", errors="ignore")
        print(f"❌ HTTP {e.code}: {details}")
        return 1
    except Exception as e:
        print(f"❌ שגיאה: {e}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
