"""
Automatic KPI report runner for Smart Traffic project.

Collects KPI metrics for a fixed duration and saves a report to disk.

KPIs:
- avg_wait (network average waiting time, seconds)
- max_queue (maximum total network queue)
- throughput (queue-clearance throughput, vehicles/min)
- emergency_response_time (seconds)

Usage:
  python python/kpi_report.py
  python python/kpi_report.py --duration 300
  python python/kpi_report.py --server http://127.0.0.1:8000 --output reports/my_kpi.json
"""

from __future__ import annotations

import argparse
import json
import time
import urllib.error
import urllib.request
from pathlib import Path
from statistics import mean
from typing import Any, Dict, List, Optional


def http_json(method: str, url: str, body: Optional[Dict[str, Any]] = None, timeout: float = 8.0) -> Dict[str, Any]:
    data = None
    headers = {"Accept": "application/json"}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"

    req = urllib.request.Request(url, method=method.upper(), data=data, headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read().decode("utf-8")
        return json.loads(raw) if raw else {}


def wait_for_metrics(server: str, timeout_sec: float = 30.0) -> Dict[str, Any]:
    deadline = time.time() + timeout_sec
    last_err: Optional[str] = None
    while time.time() < deadline:
        try:
            payload = http_json("GET", f"{server}/metrics/summary")
            if isinstance(payload, dict):
                return payload
        except Exception as exc:  # noqa: BLE001
            last_err = str(exc)
        time.sleep(1.0)
    raise RuntimeError(f"Could not fetch metrics within {timeout_sec}s. last_error={last_err}")


def pick_intersection(metrics: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    intersections = metrics.get("intersections", []) if isinstance(metrics, dict) else []
    if not intersections:
        return None
    return intersections[0]


def expected_phase_for_lane(lane_id: int) -> int:
    return 0 if (lane_id % 2 == 0) else 1


def measure_emergency_response(
    server: str,
    intersection_id: int,
    lane_id: int,
    vehicle_id: str = "AMB001",
    timeout_sec: float = 10.0,
) -> Optional[float]:
    started = time.time()
    try:
        _ = http_json(
            "POST",
            f"{server}/intersection/{intersection_id}/simulate-emergency",
            {"lane_id": lane_id, "vehicle_id": vehicle_id},
        )
    except urllib.error.HTTPError as err:
        # Manual emergency can be disabled in real hardware mode (403).
        if err.code in (400, 403, 404):
            return None
        raise

    target_phase = expected_phase_for_lane(lane_id)
    response_time: Optional[float] = None
    deadline = started + timeout_sec

    while time.time() < deadline:
        try:
            action = http_json("GET", f"{server}/intersection/{intersection_id}/action")
            if int(action.get("phase_id", -999)) == target_phase:
                response_time = time.time() - started
                break
        except Exception:
            pass
        time.sleep(0.1)

    # best-effort clear
    try:
        _ = http_json("POST", f"{server}/intersection/{intersection_id}/clear-emergency", {})
    except Exception:
        pass

    return response_time


def run_report(server: str, duration_sec: int, sample_interval_sec: float) -> Dict[str, Any]:
    metrics0 = wait_for_metrics(server)
    intersection = pick_intersection(metrics0)

    if intersection is None:
        raise RuntimeError("No intersection state available yet. Start simulation/auto-launcher first.")

    intersection_id = int(intersection.get("intersection_id", 1))
    num_lanes = max(1, int(intersection.get("num_lanes", 1)))
    lane_id = 0 if num_lanes <= 1 else min(1, num_lanes - 1)

    emergency_samples: List[float] = []

    # initial emergency response probe
    probe = measure_emergency_response(server, intersection_id, lane_id)
    if probe is not None:
        emergency_samples.append(probe)

    started_at = time.time()
    end_at = started_at + duration_sec

    wait_samples: List[float] = []
    queue_samples: List[int] = []
    queue_cleared = 0
    prev_queue: Optional[int] = None

    next_mid_probe = started_at + (duration_sec / 2.0)

    while time.time() < end_at:
        metrics = http_json("GET", f"{server}/metrics/summary")
        network_wait = float(metrics.get("avg_network_waiting_sec", 0.0))
        network_queue = int(metrics.get("total_network_queue", 0))

        wait_samples.append(network_wait)
        queue_samples.append(network_queue)

        if prev_queue is not None:
            queue_cleared += max(0, prev_queue - network_queue)
        prev_queue = network_queue

        now = time.time()
        if now >= next_mid_probe:
            probe_mid = measure_emergency_response(server, intersection_id, lane_id)
            if probe_mid is not None:
                emergency_samples.append(probe_mid)
            next_mid_probe = float("inf")

        sleep_for = max(0.0, sample_interval_sec)
        time.sleep(sleep_for)

    # final emergency response probe
    probe_end = measure_emergency_response(server, intersection_id, lane_id)
    if probe_end is not None:
        emergency_samples.append(probe_end)

    actual_duration = max(1e-6, time.time() - started_at)
    throughput_per_min = queue_cleared / (actual_duration / 60.0)

    report = {
        "generated_at": time.time(),
        "server": server,
        "duration_sec": round(actual_duration, 3),
        "sampling_interval_sec": sample_interval_sec,
        "intersection_probe": {
            "intersection_id": intersection_id,
            "lane_id": lane_id,
            "num_lanes": num_lanes,
        },
        "kpi": {
            "avg_wait": round(mean(wait_samples), 3) if wait_samples else 0.0,
            "max_queue": max(queue_samples) if queue_samples else 0,
            "throughput": round(throughput_per_min, 3),
            "emergency_response_time": round(mean(emergency_samples), 3) if emergency_samples else None,
        },
        "details": {
            "wait_samples_count": len(wait_samples),
            "queue_samples_count": len(queue_samples),
            "queue_cleared_estimate": queue_cleared,
            "emergency_response_samples": [round(v, 3) for v in emergency_samples],
            "throughput_definition": "Estimated queue-clearance throughput (sum of positive drops in total_network_queue) per minute.",
        },
    }
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate automatic KPI report for Smart Traffic")
    parser.add_argument("--server", default="http://127.0.0.1:8000", help="Base server URL")
    parser.add_argument("--duration", type=int, default=300, help="Run duration in seconds (default: 300 = 5 minutes)")
    parser.add_argument("--sample-interval", type=float, default=1.0, help="Sampling interval in seconds")
    parser.add_argument("--output", default="", help="Output JSON file path")
    args = parser.parse_args()

    report = run_report(args.server.rstrip("/"), args.duration, args.sample_interval)

    if args.output:
        out_path = Path(args.output)
    else:
        stamp = time.strftime("%Y%m%d_%H%M%S")
        out_path = Path("reports") / f"kpi_report_{stamp}.json"

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    print(f"KPI report saved: {out_path.resolve()}")
    print(json.dumps(report["kpi"], ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
