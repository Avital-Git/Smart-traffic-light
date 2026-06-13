"""
auto_launcher.py
----------------
Auto launcher for Smart Traffic Management System
Auto launcher that discovers and starts simulators for each intersection

Usage:
  --simulation   Simulation mode (no real camera, default)
  --camera       Real camera mode (requires connected camera)
"""

import sys
import time
import random
import threading
import argparse
import json
import os
from typing import Dict, List, Optional

import cv2
import numpy as np

from db_intersections import fetch_intersections
from vision.intersection_vision import (
    IntersectionAnalyzer,
    IntersectionState,
    LaneState,
    build_lane_zones,
    post_state_to_server,
)


# ══════════════════════════════════════════════════════
# סימולציית צומת (ללא מצלמה)
# ══════════════════════════════════════════════════════

class SimulatedIntersection:
    """Simulated intersection - generates random data and sends to server"""

    def __init__(self, intersection_id: int, num_lanes: int, server_url: str):
        self.intersection_id = intersection_id
        self.num_lanes = num_lanes
        self.server_url = server_url
        self.waiting_times = [0.0] * num_lanes
        self.queue_counts = [random.randint(2, 10) for _ in range(num_lanes)]
        self.width, self.height = 800, 500
        self.lane_zones = build_lane_zones(self.width, self.height, num_lanes)

    def _lane_is_green(self, action_text: str, lane_id: int) -> bool:
        if action_text == "Hold":
            return False
        if action_text == "Phase0":
            return lane_id % 2 == 0
        if action_text == "Phase1":
            return lane_id % 2 == 1
        return False

    def step(self, action_text: str = "") -> IntersectionState:
        """Generate one simulation state with dynamics affected by active phase"""
        lanes = []
        for lane_id in range(self.num_lanes):
            is_green = self._lane_is_green(action_text, lane_id)

            arrivals = random.randint(0, 3)
            departures = random.randint(2, 6) if is_green else random.randint(0, 1)

            vc = max(0, min(25, self.queue_counts[lane_id] + arrivals - departures))
            self.queue_counts[lane_id] = vc

            density = min(100.0, vc * random.uniform(7.0, 12.0))
            self.waiting_times[lane_id] = (
                self.waiting_times[lane_id] + 1.0 if vc > 0 else 0.0
            )
            lanes.append(
                LaneState(
                    lane_id=lane_id,
                    vehicle_count=vc,
                    density_pct=round(density, 1),
                    waiting_time_sec=self.waiting_times[lane_id],
                )
            )
        return IntersectionState(
            intersection_id=self.intersection_id,
            num_lanes=self.num_lanes,
            timestamp=time.time(),
            lanes=lanes,
        )

    def draw(self, state: IntersectionState, action_text: str = "") -> np.ndarray:
        """Draw a visual frame for intersection"""
        frame = np.zeros((self.height, self.width, 3), dtype=np.uint8)
        frame[:] = (30, 30, 30)

        for lane in state.lanes:
            x1, y1, x2, y2 = [int(v) for v in self.lane_zones[lane.lane_id]]
            color = (0, 180, 255) if lane.density_pct >= 70 else (0, 200, 0)
            cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
            cv2.putText(frame, f"Lane {lane.lane_id}", (x1 + 8, y1 + 22),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 2)
            cv2.putText(
                frame,
                f"Count={lane.vehicle_count} D={lane.density_pct:.1f}% W={lane.waiting_time_sec:.0f}s",
                (x1 + 8, y1 + 46), cv2.FONT_HERSHEY_SIMPLEX, 0.42, (220, 220, 220), 1,
            )
            for _ in range(min(lane.vehicle_count, 8)):
                px = random.randint(max(x1 + 10, 0), max(x1 + 11, min(x2 - 10, self.width - 1)))
                py = random.randint(max(y1 + 55, 0), max(y1 + 56, min(y2 - 10, self.height - 1)))
                cv2.circle(frame, (px, py), 5, (0, 255, 255), -1)

        header = f"Intersection #{state.intersection_id} | {state.num_lanes} lanes"
        if action_text:
            header += f" | Action: {action_text}"
        cv2.putText(frame, header, (10, 25),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)
        return frame


# ══════════════════════════════════════════════════════
# מפעיל ראשי
# ══════════════════════════════════════════════════════

class AutoLauncher:
    """Discovers intersections in SQL Server and starts simulations or analyzers"""

    def __init__(self, server_url: Optional[str] = None, use_camera: bool = False):
        self.server_url = server_url or os.environ.get(
            "STATE_ENDPOINT", "http://127.0.0.1:8000/state"
        )
        self.use_camera = use_camera
        self.intersections_config: List[dict] = []
        self.simulators: Dict[int, SimulatedIntersection] = {}
        self.analyzers: Dict[int, IntersectionAnalyzer] = {}
        self.threads = []

    def initialize_from_database(self):
        """Read intersections from SQL Server"""
        try:
            rows = fetch_intersections()
            print(f"[AUTO-LAUNCHER] OK - Found {len(rows)} intersections in SQL Server")
            for row in rows:
                iid = row["intersection_id"]
                nc = row.get("num_cameras", 4)
                name = row.get("name", "")
                print(f"  #{iid}: {name} - {nc} cameras")
                self.intersections_config.append({"id": iid, "num_cameras": nc, "name": name})
        except Exception as e:
            print(f"[AUTO-LAUNCHER] WARN - Failed to connect to DB: {e}")
            print("[AUTO-LAUNCHER] Using default - 4 intersections")
            defaults = [(1, 4), (2, 3), (3, 6), (4, 2)]
            for iid, nc in defaults:
                self.intersections_config.append({"id": iid, "num_cameras": nc, "name": f"Intersection {iid}"})

    def _run_simulation(self, sim: SimulatedIntersection, window_name: str):
        """Simulation loop for a single intersection"""
        last_action_text = ""
        while True:
            state = sim.step(last_action_text)
            resp = post_state_to_server(state, self.server_url)
            action_text = ""
            if resp:
                try:
                    data = json.loads(resp)
                    action_text = data.get("action", "")
                except Exception:
                    pass
            if action_text:
                last_action_text = action_text
            frame = sim.draw(state, action_text)
            cv2.imshow(window_name, frame)
            key = cv2.waitKey(1000) & 0xFF
            if key in (ord("q"), ord("Q")):
                break

    def _run_analyzer(self, analyzer: IntersectionAnalyzer, intersection_id: int):
        """Real analyzer loop for a single intersection"""
        print(f"[Intersection {intersection_id}] Started analysis loop")
        try:
            while True:
                state = analyzer.analyze_frame()
                if state:
                    post_state_to_server(state, self.server_url)
                    print(f"[Intersection {intersection_id}] {state.total_vehicles} vehicles")
                time.sleep(0.1)
        except KeyboardInterrupt:
            pass
        except Exception as e:
            print(f"[Intersection {intersection_id}] Error: {e}")
        finally:
            analyzer.release()

    def run(self):
        """Full execution"""
        print("=" * 60)
        print("    Smart Traffic System - Auto Launcher")
        print("=" * 60)

        self.initialize_from_database()

        if not self.intersections_config:
            print("[AUTO-LAUNCHER] No intersections to run!")
            return

        mode = "CAMERA" if self.use_camera else "SIMULATION"
        print(f"\n[AUTO-LAUNCHER] Mode: {mode}")
        print(f"[AUTO-LAUNCHER] Server: {self.server_url}")
        print(f"[AUTO-LAUNCHER] Intersections: {len(self.intersections_config)}\n")

        if self.use_camera:
            for cfg in self.intersections_config:
                iid, nc = cfg["id"], cfg["num_cameras"]
                try:
                    analyzer = IntersectionAnalyzer(
                        intersection_id=iid, num_lanes=nc,
                        camera_source=0, server_url=self.server_url,
                    )
                    self.analyzers[iid] = analyzer
                    t = threading.Thread(target=self._run_analyzer, args=(analyzer, iid), daemon=True)
                    t.start()
                    self.threads.append(t)
                except Exception as e:
                    print(f"[AUTO-LAUNCHER] Failed to create analyzer for intersection {iid}: {e}")
        else:
            for cfg in self.intersections_config:
                iid, nc = cfg["id"], cfg["num_cameras"]
                sim = SimulatedIntersection(iid, nc, self.server_url)
                self.simulators[iid] = sim
                window_name = f"Intersection #{iid} ({nc} lanes)"
                t = threading.Thread(target=self._run_simulation, args=(sim, window_name), daemon=True)
                t.start()
                self.threads.append(t)

        print("[AUTO-LAUNCHER] All intersections running. Press Q in window or Ctrl+C to stop.\n")
        try:
            for t in self.threads:
                t.join()
        except KeyboardInterrupt:
            print("\n[AUTO-LAUNCHER] Stopping.")
        finally:
            cv2.destroyAllWindows()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Smart Traffic Auto Launcher")
    parser.add_argument("--camera", action="store_true", help="Use real camera")
    parser.add_argument(
        "--server",
        default=os.environ.get("STATE_ENDPOINT", "http://127.0.0.1:8000/state"),
        help="Server /state endpoint URL (overrides $STATE_ENDPOINT)",
    )
    args = parser.parse_args()

    launcher = AutoLauncher(server_url=args.server, use_camera=args.camera)
    launcher.run()
