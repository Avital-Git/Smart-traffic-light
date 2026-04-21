"""
auto_launcher.py
----------------
מפעיל אוטומטי את מערכת ניהול התנועה.
אביטל חדד | מכללת בנות בת שבע

מטרה: לגלות אוטומטית כמה מצלמות יש בכל צומת מתוך מסד הנתונים (SQL Server),
       ולהפעיל IntersectionAnalyzer או סימולציה ויזואלית בהתאם.

מצבי הפעלה:
  --simulation   מצב סימולציה (בלי מצלמה אמיתית, ברירת מחדל)
  --camera       מצב מצלמה אמיתית (דורש מצלמה מחוברת)
"""

import sys
import time
import random
import threading
import argparse
import json
from typing import Dict, List

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
    """סימולציית צומת אחת — מייצרת נתונים אקראיים ושולחת לשרת."""

    def __init__(self, intersection_id: int, num_lanes: int, server_url: str):
        self.intersection_id = intersection_id
        self.num_lanes = num_lanes
        self.server_url = server_url
        self.waiting_times = [0.0] * num_lanes
        self.width, self.height = 800, 500
        self.lane_zones = build_lane_zones(self.width, self.height, num_lanes)

    def step(self) -> IntersectionState:
        """מחולל מצב אקראי אחד."""
        lanes = []
        for lane_id in range(self.num_lanes):
            vc = random.randint(0, 12)
            pc = random.randint(0, 4)
            density = min(100.0, vc * random.uniform(5.0, 12.0))
            self.waiting_times[lane_id] = (
                self.waiting_times[lane_id] + 1.0 if vc > 0 else 0.0
            )
            lanes.append(
                LaneState(
                    lane_id=lane_id,
                    vehicle_count=vc,
                    pedestrian_count=pc,
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
        """מציירת frame ויזואלי לצומת."""
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
                f"V={lane.vehicle_count} P={lane.pedestrian_count} "
                f"D={lane.density_pct}% W={lane.waiting_time_sec:.0f}s",
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
    """מגלה צמתים ב-SQL Server ומשגרת סימולציות או analyzers."""

    def __init__(self, server_url: str = "http://127.0.0.1:8000/state", use_camera: bool = False):
        self.server_url = server_url
        self.use_camera = use_camera
        self.intersections_config: List[dict] = []
        self.simulators: Dict[int, SimulatedIntersection] = {}
        self.analyzers: Dict[int, IntersectionAnalyzer] = {}
        self.threads = []

    def initialize_from_database(self):
        """קוראת צמתים מ-SQL Server."""
        try:
            rows = fetch_intersections()
            print(f"[AUTO-LAUNCHER] ✅ נמצאו {len(rows)} צמתים ב-SQL Server")
            for row in rows:
                iid = row["intersection_id"]
                nc = row.get("num_cameras", 4)
                name = row.get("name", "")
                print(f"  #{iid}: {name} — {nc} מצלמות")
                self.intersections_config.append({"id": iid, "num_cameras": nc, "name": name})
        except Exception as e:
            print(f"[AUTO-LAUNCHER] ⚠ לא הצלחתי להתחבר ל-DB: {e}")
            print("[AUTO-LAUNCHER] משתמשת בברירת מחדל — 4 צמתים")
            defaults = [(1, 4), (2, 3), (3, 6), (4, 2)]
            for iid, nc in defaults:
                self.intersections_config.append({"id": iid, "num_cameras": nc, "name": f"צומת {iid}"})

    def _run_simulation(self, sim: SimulatedIntersection, window_name: str):
        """לולאת סימולציה לצומת בודדת."""
        while True:
            state = sim.step()
            resp = post_state_to_server(state, self.server_url)
            action_text = ""
            if resp:
                try:
                    data = json.loads(resp)
                    action_text = data.get("action", "")
                except Exception:
                    pass
            frame = sim.draw(state, action_text)
            cv2.imshow(window_name, frame)
            key = cv2.waitKey(1000) & 0xFF
            if key in (ord("q"), ord("Q")):
                break

    def _run_analyzer(self, analyzer: IntersectionAnalyzer, intersection_id: int):
        """לולאת analyzer אמיתי לצומת בודדת."""
        print(f"[Intersection {intersection_id}] התחלתי לולאת ניתוח")
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
            print(f"[Intersection {intersection_id}] שגיאה: {e}")
        finally:
            analyzer.release()

    def run(self):
        """הפעלה מלאה."""
        print("═" * 60)
        print("    🚦 מערכת ניהול תנועה חכמה — מפעיל אוטומטי")
        print("═" * 60)

        self.initialize_from_database()

        if not self.intersections_config:
            print("[AUTO-LAUNCHER] אין צמתים להפעלה!")
            return

        mode = "CAMERA" if self.use_camera else "SIMULATION"
        print(f"\n[AUTO-LAUNCHER] מצב: {mode}")
        print(f"[AUTO-LAUNCHER] שרת: {self.server_url}")
        print(f"[AUTO-LAUNCHER] צמתים: {len(self.intersections_config)}\n")

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
                    print(f"[AUTO-LAUNCHER] נכשל ליצור analyzer לצומת {iid}: {e}")
        else:
            for cfg in self.intersections_config:
                iid, nc = cfg["id"], cfg["num_cameras"]
                sim = SimulatedIntersection(iid, nc, self.server_url)
                self.simulators[iid] = sim
                window_name = f"Intersection #{iid} ({nc} lanes)"
                t = threading.Thread(target=self._run_simulation, args=(sim, window_name), daemon=True)
                t.start()
                self.threads.append(t)

        print("[AUTO-LAUNCHER] כל הצמתים רצים. לחצי Q בחלון או Ctrl+C לעצירה.\n")
        try:
            for t in self.threads:
                t.join()
        except KeyboardInterrupt:
            print("\n[AUTO-LAUNCHER] עצירה מבוקשת.")
        finally:
            cv2.destroyAllWindows()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Smart Traffic Auto Launcher")
    parser.add_argument("--camera", action="store_true", help="שימוש במצלמה אמיתית")
    parser.add_argument("--server", default="http://127.0.0.1:8000/state", help="כתובת שרת")
    args = parser.parse_args()

    launcher = AutoLauncher(server_url=args.server, use_camera=args.camera)
    launcher.run()
