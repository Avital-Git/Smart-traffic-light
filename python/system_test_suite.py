"""
System test suite for Smart Traffic project.

What it validates:
1) API health and core state/action flow
2) Emergency auth (valid signature + replay rejection)
3) Neighbor packet signing presence
4) WebSocket live update delivery
5) Optional C++ controller roundtrip in connected mode
6) Basic stability burst (multiple state posts)

Run:
  python python/system_test_suite.py
  python python/system_test_suite.py --skip-cpp
  python python/system_test_suite.py --host 127.0.0.1 --port 8000
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import hmac
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict, List, Optional

import websockets


ROOT = Path(__file__).resolve().parents[1]
CPP_BUILD = ROOT / "cpp" / "build"


def _json_request(method: str, url: str, body: Optional[dict] = None, timeout: float = 5.0) -> tuple[int, Any]:
    data = None
    headers = {"Content-Type": "application/json"}
    if body is not None:
        data = json.dumps(body).encode("utf-8")

    req = urllib.request.Request(url=url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8")
            try:
                return resp.status, json.loads(text)
            except Exception:
                return resp.status, text
    except urllib.error.HTTPError as e:
        text = e.read().decode("utf-8", errors="ignore")
        try:
            return e.code, json.loads(text)
        except Exception:
            return e.code, text


def wait_for_health(url: str, timeout_sec: float = 20.0) -> bool:
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        status, _ = _json_request("GET", url)
        if status == 200:
            return True
        time.sleep(0.5)
    return False


def find_cpp_exe() -> Optional[Path]:
    candidates = [
        CPP_BUILD / "Debug" / "smart_traffic_controller.exe",
        CPP_BUILD / "Release" / "smart_traffic_controller.exe",
        CPP_BUILD / "smart_traffic_controller.exe",
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


def find_cpp_server_exe() -> Optional[Path]:
    candidates = [
        CPP_BUILD / "Debug" / "traffic_server.exe",
        CPP_BUILD / "Release" / "traffic_server.exe",
        CPP_BUILD / "traffic_server.exe",
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


def start_process(cmd: List[str], cwd: Path, name: str) -> subprocess.Popen:
    print(f"[TEST] start {name}: {' '.join(cmd)}")
    return subprocess.Popen(
        cmd,
        cwd=str(cwd),
        creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0),
    )


def stop_process(proc: subprocess.Popen, name: str) -> None:
    if proc.poll() is not None:
        return
    print(f"[TEST] stop {name}")
    try:
        if os.name == "nt":
            proc.send_signal(signal.CTRL_BREAK_EVENT)
            time.sleep(0.8)
        else:
            proc.terminate()
    except Exception:
        pass

    if proc.poll() is None:
        try:
            proc.terminate()
            proc.wait(timeout=3)
        except Exception:
            pass

    if proc.poll() is None:
        try:
            proc.kill()
        except Exception:
            pass


class TestRunner:
    def __init__(self) -> None:
        self.total = 0
        self.passed = 0
        self.failed: List[str] = []

    def check(self, name: str, cond: bool, detail: str = "") -> None:
        self.total += 1
        if cond:
            self.passed += 1
            print(f"[PASS] {name}")
        else:
            suffix = f" :: {detail}" if detail else ""
            print(f"[FAIL] {name}{suffix}")
            self.failed.append(name)


def build_state(intersection_id: int, ts: Optional[float] = None) -> dict:
    if ts is None:
        ts = time.time()
    return {
        "intersection_id": intersection_id,
        "num_lanes": 4,
        "timestamp": ts,
        "lanes": [
            {"lane_id": 0, "vehicle_count": 7, "density_pct": 45.0, "waiting_time_sec": 8.0},
            {"lane_id": 1, "vehicle_count": 4, "density_pct": 24.0, "waiting_time_sec": 3.0},
            {"lane_id": 2, "vehicle_count": 9, "density_pct": 61.0, "waiting_time_sec": 11.0},
            {"lane_id": 3, "vehicle_count": 2, "density_pct": 10.0, "waiting_time_sec": 1.0},
        ],
        "emergency_signal": {"active": False},
    }


def load_emergency_key() -> tuple[str, str]:
    path = ROOT / "python" / "server" / "emergency_keys.json"
    payload = json.loads(path.read_text(encoding="utf-8"))
    vehicle_id = next(iter(payload["vehicle_keys"].keys()))
    secret = payload["vehicle_keys"][vehicle_id]
    return vehicle_id, secret


def build_emergency_signature(vehicle_id: str, lane_id: int, timestamp: float, secret: str) -> str:
    canonical = f"{vehicle_id}|{lane_id}|{timestamp:.3f}"
    return hmac.new(secret.encode("utf-8"), canonical.encode("utf-8"), hashlib.sha256).hexdigest()


async def websocket_receives_state_update(ws_url: str, post_fn) -> bool:
    try:
        async with websockets.connect(ws_url, open_timeout=4, close_timeout=2) as ws:
            _ = await asyncio.wait_for(ws.recv(), timeout=4)  # welcome
            await asyncio.to_thread(post_fn)

            deadline = time.time() + 6
            while time.time() < deadline:
                msg = await asyncio.wait_for(ws.recv(), timeout=4)
                data = json.loads(msg)
                if data.get("event") == "state_updated":
                    return True
    except Exception:
        return False
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description="Smart Traffic system formal test suite")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--skip-cpp", action="store_true")
    args = parser.parse_args()

    base = f"http://{args.host}:{args.port}"
    ws_base = f"ws://{args.host}:{args.port}"

    processes: List[tuple[str, subprocess.Popen]] = []
    tr = TestRunner()

    try:
        # start C++ server
        server_exe = find_cpp_server_exe()
        if server_exe is None:
            tr.check("traffic_server executable exists", False, "traffic_server.exe not found")
            raise RuntimeError("traffic_server executable missing")

        server_cmd = [str(server_exe), str(args.port)]
        server_proc = start_process(server_cmd, server_exe.parent, "C++ traffic_server")
        processes.append(("C++ traffic_server", server_proc))

        tr.check("server health", wait_for_health(f"{base}/health"), "health endpoint did not become ready")
        if tr.failed:
            raise RuntimeError("server did not start")

        cpp_proc = None
        if not args.skip_cpp:
            cpp_exe = find_cpp_exe()
            if cpp_exe is not None:
                cpp_cmd = [str(cpp_exe), "--server", args.host, str(args.port)]
                cpp_proc = start_process(cpp_cmd, cpp_exe.parent, "C++ controller")
                processes.append(("C++ controller", cpp_proc))
            else:
                tr.check("cpp executable exists", False, "smart_traffic_controller.exe not found")

        # 1) core state flow
        state1 = build_state(1)
        status, action = _json_request("POST", f"{base}/state", state1)
        tr.check("POST /state returns 200", status == 200, f"status={status}")
        tr.check("state action format", isinstance(action, dict) and str(action.get("action", "")).startswith("Phase"), str(action))

        status, state_resp = _json_request("GET", f"{base}/intersection/1")
        tr.check("GET /intersection/1", status == 200 and isinstance(state_resp, dict), f"status={status}")

        # 2) websocket state update
        def post_for_ws():
            _json_request("POST", f"{base}/state", build_state(1, time.time()))

        ws_ok = asyncio.run(websocket_receives_state_update(f"{ws_base}/ws/intersection/1", post_for_ws))
        tr.check("WebSocket receives state_updated", ws_ok)

        # 3) emergency auth: invalid signature rejected
        bad_emergency = build_state(1, time.time())
        bad_emergency["emergency_signal"] = {
            "active": True,
            "lane_id": 1,
            "vehicle_id": "AMB001",
            "timestamp": time.time(),
            "signature": "bad-signature",
        }
        status, _ = _json_request("POST", f"{base}/state", bad_emergency)
        tr.check("invalid emergency signature rejected", status == 401, f"status={status}")

        # 4) emergency auth: valid + replay rejection
        vehicle_id, secret = load_emergency_key()
        ts = time.time()
        good_sig = build_emergency_signature(vehicle_id, 1, ts, secret)

        good_emergency = build_state(1, ts)
        good_emergency["emergency_signal"] = {
            "active": True,
            "lane_id": 1,
            "vehicle_id": vehicle_id,
            "timestamp": ts,
            "signature": good_sig,
        }
        status, _ = _json_request("POST", f"{base}/state", good_emergency)
        tr.check("valid emergency signature accepted", status == 200, f"status={status}")

        replay_status, _ = _json_request("POST", f"{base}/state", good_emergency)
        tr.check("replay emergency rejected", replay_status == 401, f"status={replay_status}")

        # 5) neighbor packet includes signed neighbor summary
        _json_request("POST", f"{base}/state", build_state(2, time.time() + 0.01))
        status, packet = _json_request("GET", f"{base}/intersection/1/packet")
        neighbors = packet.get("neighbors", []) if isinstance(packet, dict) else []
        signed_neighbor = any((n.get("signature") and n.get("signed_at", 0) > 0) for n in neighbors if isinstance(n, dict))
        tr.check("packet endpoint returns neighbors", status == 200 and isinstance(packet, dict), f"status={status}")
        tr.check("neighbor summaries are signed", signed_neighbor, f"neighbors={neighbors}")

        # 6) metrics summary sanity
        status, metrics = _json_request("GET", f"{base}/metrics/summary")
        tr.check("metrics summary endpoint", status == 200 and isinstance(metrics, dict), f"status={status}")
        tr.check("metrics has intersections", isinstance(metrics.get("intersections"), list), str(metrics))

        # 7) optional cpp roundtrip check
        if cpp_proc is not None:
            deadline = time.time() + 12
            cpp_action_seen = False
            while time.time() < deadline:
                a_status, a_payload = _json_request("GET", f"{base}/intersection/1/action")
                if a_status == 200 and isinstance(a_payload, dict):
                    reason = str(a_payload.get("reason", ""))
                    if "cpp" in reason.lower():
                        cpp_action_seen = True
                        break
                time.sleep(1.0)
            tr.check("cpp controller updates action", cpp_action_seen)

        # 8) burst stability
        burst_ok = True
        for i in range(12):
            s, _ = _json_request("POST", f"{base}/state", build_state(1, time.time() + i * 0.001))
            if s != 200:
                burst_ok = False
                break
        tr.check("burst POST /state stability", burst_ok)

    except Exception as exc:
        print(f"[TEST] fatal error: {exc}")
    finally:
        for name, proc in reversed(processes):
            stop_process(proc, name)

    print("\n=== SYSTEM TEST SUMMARY ===")
    print(f"passed: {tr.passed}/{tr.total}")
    if tr.failed:
        print("failed tests:")
        for name in tr.failed:
            print(f" - {name}")
        return 1

    print("all tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
