"""
intersection_vision.py
-----------------------
מודול עיבוד תמונה — מערכת רמזורים חכמה
אביטל חדד | מכללת בנות בת שבע
-------------------------------------------------

מודול זה תומך בצמתים דינאמיים עם מספר משתנה של נתיבים.
כל צומת יכולה להיות בעלת 2, 3, 4 או יותר מצלמות/נתיבים.

מה המודול הזה עושה:
  ✅ ספירת כלי רכב בכל נתיב (לפי מספר המצלמות)     — YOLO
  ✅ זיהוי הולכי רגל בכל נתיב                    — YOLO
  ✅ חישוב צפיפות תנועה (אחוז עומס)              — Background Subtraction + Traffic Density Estimation
  ✅ ביטול בקשות שהעצם נעלם                        — בדיקה בפריים הבא
  ✅ RL vector דינאמי בהתאם למספר הנתיבים        — to_rl_vector()

קלט:  מספר נתיבים, פריים וידאו מהמצלמה
פלט:  IntersectionState — וקטור מצב מלא שנכנס לאלגוריתם RL

תלויות:
    pip install ultralytics opencv-python numpy
"""

import cv2
import numpy as np
import time
import json
import urllib.request
import urllib.error
from dataclasses import dataclass, asdict, field
from typing import Optional, List


# ══════════════════════════════════════════════════════
# מבני נתונים
# ══════════════════════════════════════════════════════

@dataclass
class GPSEmergencySignal:
    """
    אות חירום המגיע מלחצן ה-GPS ברכב החירום.
    לפי ההצעה:
      "לחיצה על הרכיב שולחת בקשה לבקר ויינתן אור ירוק
       לנתיב בו נמצא רכב החירום בזמן הכי מהיר שאפשר."
    """
    active: bool
    lane_id: int = 0            # מזהה הנתיב (0, 1, 2, ...)
    vehicle_id: str = ""
    timestamp: float = 0.0


@dataclass
class LaneState:
    """מצב נתיב יחיד - כעת עם lane_id דינאמי."""
    lane_id: int
    vehicle_count: int
    pedestrian_count: int
    density_pct: float
    waiting_time_sec: float = 0.0


@dataclass
class IntersectionState:
    """
    המצב המלא של צומת עם תמיכה בנתיבים דינאמיים.
    """
    intersection_id: int
    num_lanes: int
    timestamp: float
    lanes: List[LaneState]
    emergency_signal: Optional[GPSEmergencySignal] = None
    neighbor_states: dict = field(default_factory=dict)

    @property
    def total_vehicles(self) -> int:
        return sum(lane.vehicle_count for lane in self.lanes)

    def to_rl_vector(self) -> list:
        """
        ממיר את כל ה-State לוקטור מספרי לרשת הנוירונים.
        ווקטור דינאמי: (4 * num_lanes + 1) ערכים.
        """
        vector = []
        for lane in self.lanes:
            vector.append(float(lane.vehicle_count))
        for lane in self.lanes:
            vector.append(float(lane.pedestrian_count))
        for lane in self.lanes:
            vector.append(lane.density_pct / 100.0)
        for lane in self.lanes:
            vector.append(lane.waiting_time_sec)
        
        emergency_active = (
            self.emergency_signal is not None and self.emergency_signal.active
        )
        vector.append(1.0 if emergency_active else 0.0)
        return vector

    def to_json(self) -> str:
        """ממיר ל-JSON לשליחה לשרת."""
        data = {
            "intersection_id": self.intersection_id,
            "num_lanes": self.num_lanes,
            "timestamp": self.timestamp,
            "lanes": [asdict(lane) for lane in self.lanes],
            "emergency_active": (
                self.emergency_signal.active
                if self.emergency_signal else False
            ),
            "emergency_lane_id": (
                self.emergency_signal.lane_id
                if self.emergency_signal and self.emergency_signal.active else None
            ),
            "total_vehicles": self.total_vehicles,
        }
        return json.dumps(data, ensure_ascii=False)


def post_state_to_server(state: IntersectionState, server_url: str = "http://127.0.0.1:8000/state") -> Optional[str]:
    """שולח מצב לשרת FastAPI."""
    try:
        data = state.to_json().encode("utf-8")
        request = urllib.request.Request(
            server_url,
            data=data,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.read().decode("utf-8")
    except urllib.error.URLError as e:
        print(f"[Intersection {state.intersection_id}] שגיאת חיבור: {e}")
        return None
    except Exception as e:
        print(f"[Intersection {state.intersection_id}] שגיאת שליחה: {e}")
        return None


# ══════════════════════════════════════════════════════
# בניית אזורי נתיבים דינאמיים
# ══════════════════════════════════════════════════════

def build_lane_zones(frame_width: int, frame_height: int, num_lanes: int) -> List[tuple]:
    """
    בונה רשימה של ROI (אזורי עניין) לכל נתיב.
    מאפשרת צמתים עם מספר משתנה של נתיבים.
    """
    w, h = frame_width, frame_height
    
    if num_lanes == 4:
        cx, cy = w // 2, h // 2
        return [
            (cx - 100, 0, cx + 100, cy - 20),           # lane 0: North
            (cx - 100, cy + 20, cx + 100, h),           # lane 1: South
            (0, cy - 100, cx - 20, cy + 100),           # lane 2: West
            (cx + 20, cy - 100, w, cy + 100),           # lane 3: East
        ]
    elif num_lanes == 3:
        cx, cy = w // 2, h // 2
        return [
            (cx - 100, 0, cx + 100, cy - 20),           # lane 0: Top
            (0, cy - 50, cx - 20, h),                   # lane 1: Bottom-Left
            (cx + 20, cy - 50, w, h),                   # lane 2: Bottom-Right
        ]
    elif num_lanes == 2:
        cx, cy = w // 2, h // 2
        return [
            (cx - 100, 0, cx + 100, cy - 20),           # lane 0: Top
            (cx - 100, cy + 20, cx + 100, h),           # lane 1: Bottom
        ]
    else:
        # חלוקה ליניארית לכל מספר אחר
        zones = []
        lane_height = h // num_lanes
        for i in range(num_lanes):
            y1 = i * lane_height
            y2 = (i + 1) * lane_height
            zones.append((0, y1, w, y2))
        return zones


# ══════════════════════════════════════════════════════
# Background Subtraction — צפיפות תנועה
# ══════════════════════════════════════════════════════

class TrafficDensityEstimator:
    """אלגוריתם Background Subtraction לחישוב צפיפות עומס."""

    def __init__(self):
        self.bg_subtractor = cv2.createBackgroundSubtractorMOG2(
            history=500,
            varThreshold=50,
            detectShadows=True
        )

    def compute_density(self, zone_frame: np.ndarray) -> float:
        """מחזירה אחוז עומס 0-100."""
        if zone_frame.size == 0:
            return 0.0
        
        fg_mask = self.bg_subtractor.apply(zone_frame)
        _, fg_mask = cv2.threshold(fg_mask, 200, 255, cv2.THRESH_BINARY)
        
        kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
        fg_mask = cv2.morphologyEx(fg_mask, cv2.MORPH_CLOSE, kernel)
        fg_mask = cv2.morphologyEx(fg_mask, cv2.MORPH_OPEN, kernel)
        
        total_pixels = fg_mask.shape[0] * fg_mask.shape[1]
        occupied_pixels = cv2.countNonZero(fg_mask)
        
        if total_pixels == 0:
            return 0.0
        
        density = (occupied_pixels / total_pixels) * 100.0
        return round(min(density, 100.0), 1)


# ══════════════════════════════════════════════════════
# YOLO — זיהוי רכבים והולכי רגל
# ══════════════════════════════════════════════════════

VEHICLE_CLASS_IDS = {2, 3, 5, 7}      # car, motorcycle, bus, truck
PEDESTRIAN_CLASS_ID = 0                # person


# ══════════════════════════════════════════════════════
# סוכן צומת ראשי
# ══════════════════════════════════════════════════════

class IntersectionAnalyzer:
    """סוכן צומת עצמאי עם תמיכה בנתיבים דינאמיים."""

    def __init__(
        self,
        intersection_id: int,
        num_lanes: int,
        camera_source,
        model_path: str = "yolov8n.pt",
        confidence: float = 0.45,
        sample_interval_sec: float = 2.0,
        server_url: Optional[str] = None,
    ):
        self.intersection_id = intersection_id
        self.num_lanes = num_lanes
        self.confidence = confidence
        self.sample_interval = sample_interval_sec
        self._last_sample_time = 0.0
        self.server_url = server_url
        self._emergency_signal: Optional[GPSEmergencySignal] = None
        self._waiting_times = [0.0] * num_lanes

        from ultralytics import YOLO
        print(f"[Intersection {intersection_id}] טוען YOLO...")
        self.model = YOLO(model_path)

        self.cap = cv2.VideoCapture(camera_source)
        if not self.cap.isOpened():
            raise RuntimeError(f"לא ניתן לפתוח מקור וידאו: {camera_source}")

        ret, frame = self.cap.read()
        if not ret:
            raise RuntimeError("לא ניתן לקרוא פריים ראשון")
        
        h, w = frame.shape[:2]
        self.lane_zones = build_lane_zones(w, h, num_lanes)
        self.density_estimators = [TrafficDensityEstimator() for _ in range(num_lanes)]

        print(f"[Intersection {intersection_id}] מוכן | {w}x{h} | {num_lanes} נתיבים")

    def set_emergency_signal(self, signal: GPSEmergencySignal):
        """קבלת אות GPS מהבקר ב-C++."""
        self._emergency_signal = signal
        if signal.active:
            print(f"[Intersection {self.intersection_id}] GPS EMERGENCY! lane {signal.lane_id}")

    def clear_emergency_signal(self):
        """ניקוי אות חירום."""
        self._emergency_signal = None

    def analyze_frame(self) -> Optional[IntersectionState]:
        """קריאה ועיבוד פריים."""
        now = time.time()
        if now - self._last_sample_time < self.sample_interval:
            return None

        ret, frame = self.cap.read()
        if not ret:
            return None

        self._last_sample_time = now
        state = self._process_frame(frame, now)
        
        if self.server_url and state is not None:
            post_state_to_server(state, self.server_url)
        
        return state

    def _process_frame(self, frame: np.ndarray, timestamp: float) -> IntersectionState:
        """עיבוד בפועל."""
        results = self.model(frame, conf=self.confidence, verbose=False)[0]
        detections = self._parse_detections(results)

        lanes = []
        for lane_id in range(self.num_lanes):
            x1, y1, x2, y2 = self.lane_zones[lane_id]
            x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)

            vehicles, pedestrians = 0, 0
            for det in detections:
                cx, cy = det["cx"], det["cy"]
                if x1 <= cx <= x2 and y1 <= cy <= y2:
                    if det["class_id"] in VEHICLE_CLASS_IDS:
                        vehicles += 1
                    elif det["class_id"] == PEDESTRIAN_CLASS_ID:
                        pedestrians += 1

            zone_roi = frame[y1:y2, x1:x2]
            density = (
                self.density_estimators[lane_id].compute_density(zone_roi)
                if zone_roi.size > 0 else 0.0
            )

            if vehicles > 0:
                self._waiting_times[lane_id] += self.sample_interval
            else:
                self._waiting_times[lane_id] = 0.0

            lanes.append(LaneState(
                lane_id=lane_id,
                vehicle_count=vehicles,
                pedestrian_count=pedestrians,
                density_pct=density,
                waiting_time_sec=self._waiting_times[lane_id],
            ))

        return IntersectionState(
            intersection_id=self.intersection_id,
            num_lanes=self.num_lanes,
            timestamp=timestamp,
            lanes=lanes,
            emergency_signal=self._emergency_signal,
        )

    def _parse_detections(self, results) -> list:
        """חילוץ DetectionBox מ-YOLO."""
        detections = []
        if results.boxes is None:
            return detections
        for box in results.boxes:
            x1, y1, x2, y2 = box.xyxy[0].tolist()
            detections.append({
                "class_id": int(box.cls[0]),
                "cx": (x1 + x2) / 2,
                "cy": (y1 + y2) / 2,
            })
        return detections

    def visualize(self, frame: np.ndarray, state: IntersectionState) -> np.ndarray:
        """ציור debug על הפריים."""
        vis = frame.copy()
        colors = [(0, 220, 0), (255, 120, 0), (0, 120, 255), (220, 220, 0)]

        for lane_id, (x1, y1, x2, y2) in enumerate(self.lane_zones):
            x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)
            lane = state.lanes[lane_id]
            color = colors[lane_id % len(colors)]
            cv2.rectangle(vis, (x1, y1), (x2, y2), color, 2)
            
            lines = [
                f"Lane {lane_id}: {lane.vehicle_count} vehicles | {lane.pedestrian_count} pedestrians",
                f"Density: {lane.density_pct}% | Waiting: {lane.waiting_time_sec:.0f}s",
            ]
            for i, text in enumerate(lines):
                cv2.putText(vis, text, (x1 + 5, y1 + 20 + i * 20),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 2)

        if state.emergency_signal and state.emergency_signal.active:
            msg = f"GPS EMERGENCY -- Lane {state.emergency_signal.lane_id}"
            cv2.rectangle(vis, (0, 0), (vis.shape[1], 50), (0, 0, 200), -1)
            cv2.putText(vis, msg, (10, 35),
                        cv2.FONT_HERSHEY_SIMPLEX, 1.0, (255, 255, 255), 2)

        return vis

    def release(self):
        self.cap.release()

    def __del__(self):
        self.release()


# ══════════════════════════════════════════════════════
# בדיקה
# ══════════════════════════════════════════════════════

if __name__ == "__main__":
    print("=== Vision Module Test ===")
    print("Q = exit | E = simulate emergency | 1-4 = change lanes")

    # בדיקה עם 3 נתיבים
    analyzer = IntersectionAnalyzer(
        intersection_id=1,
        num_lanes=3,
        camera_source=0,
        sample_interval_sec=1.0,
        server_url="http://127.0.0.1:8000/state",
    )

    while True:
        ret, frame = analyzer.cap.read()
        if not ret:
            break

        state = analyzer.analyze_frame()
        if state:
            print(f"\n[{time.strftime('%H:%M:%S')}] Intersection {state.intersection_id}")
            for lane in state.lanes:
                print(f"  Lane {lane.lane_id}: {lane.vehicle_count} vehicles | "
                      f"{lane.pedestrian_count} pedestrians | "
                      f"Density {lane.density_pct}% | "
                      f"Waiting {lane.waiting_time_sec:.0f}s")
            print(f"  RL vector: {state.to_rl_vector()}")

            vis = analyzer.visualize(frame, state)
            cv2.imshow("Intersection Analyzer", vis)

        key = cv2.waitKey(1) & 0xFF
        if key == ord('q'):
            break
        elif key == ord('e'):
            analyzer.set_emergency_signal(GPSEmergencySignal(
                active=True,
                lane_id=1,
                vehicle_id="AMBULANCE_001",
                timestamp=time.time(),
            ))

    analyzer.release()
    cv2.destroyAllWindows()
