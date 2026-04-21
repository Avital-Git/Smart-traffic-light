"""
intersection_vision.py
-----------------------
מודול עיבוד תמונה — מערכת רמזורים חכמה
אביטל חדד | מכללת בנות בת שבע
-------------------------------------------------
 
מה המודול הזה עושה (לפי הצעת הפרויקט):
  ✅ ספירת כלי רכב בכל נתיב          — YOLO
  ✅ זיהוי הולכי רגל בכל נתיב         — YOLO
  ✅ חישוב צפיפות תנועה (אחוז עומס)  — Background Subtraction + Traffic Density Estimation
  ✅ ביטול בקשות שהעצם נעלם           — בדיקה בפריים הבא
 
מה המודול הזה לא עושה (כי כתבת אחרת בהצעה):
  ❌ זיהוי רכב חירום מהתמונה
     -> רכב חירום מזוהה ע"י לחצן GPS מיוחד ברכב
     -> האות מגיע ישירות לבקר הצומת ב-C++
     -> ראי: GPSEmergencySignal בקובץ זה — רק מבנה נתונים לממשק
 
קלט:  פריים וידאו מהמצלמה שמסריטה את הצומת
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
from typing import Optional
 
 
# ══════════════════════════════════════════════════════
# מבני נתונים
# ══════════════════════════════════════════════════════
 
@dataclass
class GPSEmergencySignal:
    """
    אות חירום המגיע מלחצן ה-GPS ברכב החירום.
    המבנה הזה מגיע מהבקר ב-C++ — לא מהמצלמה.
 
    לפי ההצעה:
      "לחיצה על הרכיב שולחת בקשה לבקר ויינתן אור ירוק
       לנתיב בו נמצא רכב החירום בזמן הכי מהיר שאפשר."
    """
    active: bool            # האם יש כרגע בקשת חירום פעילה
    lane_direction: str     # "N" | "S" | "E" | "W" — הנתיב לפי מיקום ה-GPS
    vehicle_id: str = ""    # מזהה הרכב (לאימות חתימה דיגיטלית — אבטחת מידע)
    timestamp: float = 0.0  # זמן קבלת האות
 
 
@dataclass
class LaneState:
    """מצב נתיב יחיד."""
    lane_id: int            # מזהה נתיב ייחודי בצומת (0, 1, 2, ...)
    vehicle_count: int      # מספר כלי רכב (מ-YOLO)
    pedestrian_count: int   # מספר הולכי רגל (מ-YOLO)
    density_pct: float      # אחוז עומס 0.0-100.0 (מ-Background Subtraction)
    waiting_time_sec: float = 0.0  # זמן המתנה מצטבר — לחישוב Reward ב-RL
 
 
@dataclass
class IntersectionState:
    """
    המצב המלא של צומת — זה ה-State שנכנס לאלגוריתם RL.
 
    3 מקורות קלט לפי ההצעה:
      1. תמונת וידאו — מעובד כאן ב-Python (עבור כל מצלמה)
      2. אות GPS חירום — מגיע מ-C++ (emergency_signal)
      3. נתוני צמתים סמוכים — מגיע מ-C++ (neighbor_states)
    """
    intersection_id: int
    num_lanes: int          # מספר המצלמות/נתיבים בצומת
    timestamp: float
    lanes: list             # רשימה של LaneState, אורך = num_lanes
    emergency_signal: GPSEmergencySignal = None
    neighbor_states: dict = field(default_factory=dict)

    @property
    def total_vehicles(self) -> int:
        return sum(lane.vehicle_count for lane in self.lanes)

    def to_rl_vector(self) -> list:
        """
        ממיר את כל ה-State לוקטור מספרי לרשת הנוירונים.
        ווקטור דינאמי בהתאם למספר נתיבים.

        מבנה הוקטור (4*num_lanes + 1):
          [0...num_lanes-1]         vehicle_count לכל נתיב
          [num_lanes...2*num_lanes-1] pedestrian_count לכל נתיב
          [2*num_lanes...3*num_lanes-1] density_pct מנורמל 0-1 לכל נתיב
          [3*num_lanes...4*num_lanes-1] waiting_time_sec לכל נתיב
          [4*num_lanes]               emergency_flag (1.0 אם יש אות GPS פעיל)
        """
        vector = []
        
        # vehicle_count
        for lane in self.lanes:
            vector.append(float(lane.vehicle_count))
        
        # pedestrian_count
        for lane in self.lanes:
            vector.append(float(lane.pedestrian_count))
        
        # density_pct (מנורמל)
        for lane in self.lanes:
            vector.append(lane.density_pct / 100.0)
        
        # waiting_time_sec
        for lane in self.lanes:
            vector.append(lane.waiting_time_sec)
        
        # emergency signal
        emergency_active = (
            self.emergency_signal is not None and self.emergency_signal.active
        )
        vector.append(1.0 if emergency_active else 0.0)
        
        return vector

    def to_json(self) -> str:
        """לשליחה בין צמתים דרך TCP/IP או MQTT (לפי ההצעה)."""
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
                self.emergency_signal.lane_direction
                if self.emergency_signal and self.emergency_signal.active else None
            ),
            "total_vehicles": self.total_vehicles,
        }
        return json.dumps(data, ensure_ascii=False)


def post_state_to_server(state: IntersectionState, server_url: str = "http://127.0.0.1:8000/state") -> Optional[str]:
    """
    שולח את מצב הצומת לשרת FastAPI.
    זה מאפשר לקשר בין מודול ה-vision לבין שרת ההחלטות.
    """
    try:
        data = state.to_json().encode("utf-8")
        request = urllib.request.Request(
            server_url,
            data=data,
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.read().decode("utf-8")
    except urllib.error.URLError as e:
        print(f"[Intersection {state.intersection_id}] שגיאת חיבור לשרת: {e}")
        return None
    except Exception as e:
        print(f"[Intersection {state.intersection_id}] שגיאת שליחה לשרת: {e}")
        return None

# ══════════════════════════════════════════════════════
# הגדרת אזורי הנתיבים (ROI)
# ══════════════════════════════════════════════════════

def build_lane_zones(frame_width: int, frame_height: int, num_lanes: int) -> list:
    """
    מחלקת את הפריים ל-num_lanes אזורי עניין.
    כל lens מקבל ROI משלו בהתאם למספר הנתיבים.

    מחזירה: רשימה של (x1, y1, x2, y2) של גודל num_lanes
    """
    w, h = frame_width, frame_height
    cx, cy = w // 2, h // 2
    
    if num_lanes == 4:
        # ברירת מחדל: 4 כיוונים (N, S, E, W)
        return [
            (cx - 100, 0,        cx + 100, cy - 20),      # North
            (cx - 100, cy + 20,  cx + 100, h),            # South
            (0,         cy - 100, cx - 20,  cy + 100),    # West
            (cx + 20,   cy - 100, w,        cy + 100),    # East
        ]
    elif num_lanes == 3:
        # 3 כיוונים - משולש
        return [
            (cx - 100, 0,        cx + 100, cy - 20),      # Top
            (0,         cy - 50,  cx - 20,  h),           # Bottom Left
            (cx + 20,   cy - 50,  w,        h),           # Bottom Right
        ]
    elif num_lanes == 2:
        # 2 כיוונים - ישרים (למשל North-South)
        return [
            (cx - 100, 0,        cx + 100, cy - 20),      # Top
            (cx - 100, cy + 20,  cx + 100, h),            # Bottom
        ]
    else:
        # מופרד דינאמי לכל מספר אחר
        angle_step = 360.0 / num_lanes
        zones = []
        for i in range(num_lanes):
            # חלוקה זוויתית של הפריים
            # זה הוא קירוב פשוט - בפועל יהיה צורך בכיול אמיתי לפי סידור המצלמות
            start_angle = i * angle_step
            end_angle = (i + 1) * angle_step
            
            # חישוב ROI מרובע בנוח (פשוט לבדיקה)
            x1 = int(cx - 100)
            y1 = int(cy - 100 + (i * (h // num_lanes)))
            x2 = int(cx + 100)
            y2 = int(cy - 100 + ((i + 1) * (h // num_lanes)))
            
            zones.append((max(0, x1), max(0, y1), min(w, x2), min(h, y2)))
        
        return zones
 
 
# ══════════════════════════════════════════════════════
# Background Subtraction — לחישוב צפיפות תנועה
# ══════════════════════════════════════════════════════
 
class TrafficDensityEstimator:
    """
    אלגוריתם Background Subtraction לחישוב אחוז עומס בנתיב.
 
    לפי ההצעה:
      "Background Subtraction — האלגוריתם יוצר תמונת רקע סטטית
       של הכביש וכל פריים חדש נבדק מול הרקע, פיקסלים ששונים
       מהרקע מסומנים כעצמים נעים."
 
      "Traffic Density Estimation — מקבל מידע מ-Background Subtraction,
       מחשב את אחוז הכביש המוצף ברכבים ונותן מדד כמותי למצב התנועה."
    """
 
    def __init__(self):
        # MOG2 — Background Subtractor מובנה ב-OpenCV
        # מתאים לשינויי תאורה בין יום ולילה (חסרון שציינת בהצעה)
        self.bg_subtractor = cv2.createBackgroundSubtractorMOG2(
            history=500,        # כמה פריימים לזכור לבניית הרקע
            varThreshold=50,    # רגישות — ערך גבוה = פחות רעש
            detectShadows=True  # מסנן צללים (בעיה שציינת בהצעה)
        )
 
    def compute_density(self, zone_frame: np.ndarray) -> float:
        """
        מקבלת תמונת נתיב (ROI), מחזירה אחוז עומס 0.0-100.0.
 
        שלבים:
          1. Background Subtraction — מה שונה מהרקע = עצם נע
          2. סינון צללים (ערך 127 ב-MOG2 = צל, לא עצם)
          3. Morphology — ניקוי רעש קטן
          4. חישוב אחוז פיקסלים תפוסים = צפיפות
        """
        # שלב 1: חישוב Foreground Mask
        fg_mask = self.bg_subtractor.apply(zone_frame)
 
        # שלב 2: הסרת צללים (ערך 127) — רק עצמים ממש (ערך 255)
        _, fg_mask = cv2.threshold(fg_mask, 200, 255, cv2.THRESH_BINARY)
 
        # שלב 3: Morphology — סגירת חורים קטנים וניקוי רעש
        kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
        fg_mask = cv2.morphologyEx(fg_mask, cv2.MORPH_CLOSE, kernel)
        fg_mask = cv2.morphologyEx(fg_mask, cv2.MORPH_OPEN, kernel)
 
        # שלב 4: אחוז פיקסלים לבנים (עצמים) מסך הנתיב
        total_pixels = fg_mask.shape[0] * fg_mask.shape[1]
        occupied_pixels = cv2.countNonZero(fg_mask)
 
        if total_pixels == 0:
            return 0.0
 
        density = (occupied_pixels / total_pixels) * 100.0
        return round(min(density, 100.0), 1)
 
 
# ══════════════════════════════════════════════════════
# YOLO — לספירת רכבים ולזיהוי הולכי רגל
# ══════════════════════════════════════════════════════
 
# COCO class IDs
VEHICLE_CLASS_IDS   = {2, 3, 5, 7}   # car, motorcycle, bus, truck
PEDESTRIAN_CLASS_ID = 0               # person
 
# הערה: אין כאן זיהוי רכב חירום מהתמונה!
# זיהוי חירום = לחצן GPS בלבד, לפי ההצעה.
 
 
# ══════════════════════════════════════════════════════
# מחלקה ראשית — סוכן צומת
# ══════════════════════════════════════════════════════
 
class IntersectionAnalyzer:
    """
    סוכן עצמאי לצומת בודדת — לפי עקרון Multi-Agent בהצעה.
    כל צומת מריץ instance נפרד, עצמאי.
 
    מקבל:
      - פריימים מהמצלמה (עיבוד תמונה — Python)
      - אות GPS חירום (מהבקר ב-C++ דרך set_emergency_signal)
 
    מחזיר:
      - IntersectionState מלא לאלגוריתם RL
    """
 
    def __init__(
        self,
        intersection_id: int,
        camera_source,
        model_path: str = "yolov8n.pt",
        confidence: float = 0.45,
        sample_interval_sec: float = 2.0,
        server_url: Optional[str] = None,
    ):
        self.intersection_id = intersection_id
        self.confidence = confidence
        self.sample_interval = sample_interval_sec
        self._last_sample_time = 0.0
        self.server_url = server_url
        # טעינת YOLO
        from ultralytics import YOLO
        print(f"[Intersection {intersection_id}] טוען YOLO...")
        self.model = YOLO(model_path)
 
        # פתיחת מצלמה
        self.cap = cv2.VideoCapture(camera_source)
        if not self.cap.isOpened():
            raise RuntimeError(f"לא ניתן לפתוח מקור וידאו: {camera_source}")
 
        ret, frame = self.cap.read()
        if not ret:
            raise RuntimeError("לא ניתן לקרוא פריים ראשון.")
        h, w = frame.shape[:2]
        self.lane_zones = build_lane_zones(w, h)
 
        # Background Subtraction — מופע נפרד לכל נתיב (מדויק יותר)
        self.density_estimators = {
            direction: TrafficDensityEstimator()
            for direction in ["N", "S", "E", "W"]
        }
 
        print(f"[Intersection {intersection_id}] מוכן | גודל פריים: {w}x{h}")
 
    # ──────────────────────────────────────────────
    # קלט חירום מהבקר ב-C++ (לא מהמצלמה)
    # ──────────────────────────────────────────────
 
    def set_emergency_signal(self, signal: GPSEmergencySignal):
        """
        מקבל אות GPS מהבקר ב-C++.
        נקרא כשרכב חירום לוחץ על הלחצן ושולח את מיקומו.
 
        לפי ההצעה: "לחיצה על הרכיב שולחת בקשה לבקר
        ויינתן אור ירוק לנתיב בו נמצא רכב החירום."
        """
        self._emergency_signal = signal
        if signal.active:
            print(f"[Intersection {self.intersection_id}] "
                  f"GPS EMERGENCY! נתיב: {signal.lane_direction} | "
                  f"רכב: {signal.vehicle_id}")
 
    def clear_emergency_signal(self):
        """מנקה את האות אחרי שהרכב עבר — ביטול אוטומטי לפי ההצעה."""
        self._emergency_signal = None
 
    # ──────────────────────────────────────────────
    # פונקציה ראשית
    # ──────────────────────────────────────────────
 
    def analyze_frame(self) -> Optional[IntersectionState]:
        """
        קוראת פריים, מעבדת, מחזירה IntersectionState.
        מחזירה None אם עדיין לא הגיע זמן הדגימה.
        """
        now = time.time()
        if now - self._last_sample_time < self.sample_interval:
            return None
 
        ret, frame = self.cap.read()
        if not ret:
            print(f"[Intersection {self.intersection_id}] אזהרה: פריים ריק.")
            return None
 
        self._last_sample_time = now
        state = self._process_frame(frame, now)
        if self.server_url and state is not None:
            post_state_to_server(state, self.server_url)
        return state
 
    # ──────────────────────────────────────────────
    # עיבוד פנימי
    # ──────────────────────────────────────────────
 
    def _process_frame(self, frame: np.ndarray, timestamp: float) -> IntersectionState:
        """
        שני אלגוריתמים במקביל על אותו פריים:
          1. YOLO        — ספירת רכבים + הולכי רגל
          2. Background Subtraction + Traffic Density — אחוז עומס
        """
        # YOLO על הפריים המלא
        results = self.model(frame, conf=self.confidence, verbose=False)[0]
        detections = self._parse_detections(results)
 
        lanes = {}
        for direction, (x1, y1, x2, y2) in self.lane_zones.items():
            x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)
 
            # ספירת רכבים והולכי רגל עם YOLO
            vehicles, pedestrians = 0, 0
            for det in detections:
                cx, cy = det["cx"], det["cy"]
                if x1 <= cx <= x2 and y1 <= cy <= y2:
                    if det["class_id"] in VEHICLE_CLASS_IDS:
                        vehicles += 1
                    elif det["class_id"] == PEDESTRIAN_CLASS_ID:
                        pedestrians += 1
 
            # Background Subtraction על ROI הנתיב בלבד
            zone_roi = frame[y1:y2, x1:x2]
            density = (
                self.density_estimators[direction].compute_density(zone_roi)
                if zone_roi.size > 0 else 0.0
            )
 
            # עדכון זמן המתנה לחישוב Reward ב-RL
            if vehicles > 0:
                self._waiting_times[direction] += self.sample_interval
            else:
                self._waiting_times[direction] = 0.0
 
            lanes[direction] = LaneState(
                direction=direction,
                vehicle_count=vehicles,
                pedestrian_count=pedestrians,
                density_pct=density,
                waiting_time_sec=self._waiting_times[direction],
            )
 
        return IntersectionState(
            intersection_id=self.intersection_id,
            timestamp=timestamp,
            lanes=lanes,
            emergency_signal=self._emergency_signal,  # מה-GPS, לא מהמצלמה
        )
 
    def _parse_detections(self, results) -> list:
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
 
    # ──────────────────────────────────────────────
    # ויזואליזציה ל-Debug
    # ──────────────────────────────────────────────
 
    def visualize(self, frame: np.ndarray, state: IntersectionState) -> np.ndarray:
        """מצייר אזורי נתיבים + נתוני מצב על הפריים לצורך פיתוח ובדיקות."""
        vis = frame.copy()
        colors = {"N": (0, 220, 0), "S": (255, 120, 0),
                  "E": (0, 120, 255), "W": (220, 220, 0)}
 
        for direction, (x1, y1, x2, y2) in self.lane_zones.items():
            x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)
            lane = state.lanes[direction]
            color = colors[direction]
            cv2.rectangle(vis, (x1, y1), (x2, y2), color, 2)
            lines = [
                f"{direction}: {lane.vehicle_count} רכב | {lane.pedestrian_count} הולכי רגל",
                f"עומס: {lane.density_pct}% | המתנה: {lane.waiting_time_sec:.0f}s",
            ]
            for i, text in enumerate(lines):
                cv2.putText(vis, text, (x1 + 5, y1 + 20 + i * 20),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 2)
 
        # חירום GPS — מוצג רק אם האות פעיל (לא מהמצלמה!)
        if state.emergency_signal and state.emergency_signal.active:
            msg = f"GPS EMERGENCY -- נתיב {state.emergency_signal.lane_direction}"
            cv2.rectangle(vis, (0, 0), (vis.shape[1], 50), (0, 0, 200), -1)
            cv2.putText(vis, msg, (10, 35),
                        cv2.FONT_HERSHEY_SIMPLEX, 1.0, (255, 255, 255), 2)
 
        return vis
 
    def release(self):
        self.cap.release()
 
    def __del__(self):
        self.release()
 
 
# ══════════════════════════════════════════════════════
# Multi-Agent — הרצת כמה צמתים במקביל
# ══════════════════════════════════════════════════════
 
def run_multi_agent(camera_sources: list, on_state_update=None):
    """
    מריצה סוכן עצמאי לכל צומת — כל אחד ב-Thread נפרד.
    לפי עקרון Multi-Agent בהצעה.
    """
    import threading
 
    analyzers = [
        IntersectionAnalyzer(intersection_id=i, camera_source=src)
        for i, src in enumerate(camera_sources)
    ]
 
    def agent_loop(analyzer: IntersectionAnalyzer):
        while True:
            state = analyzer.analyze_frame()
            if state and on_state_update:
                on_state_update(state)
            time.sleep(0.1)
 
    threads = [
        threading.Thread(target=agent_loop, args=(a,), daemon=True)
        for a in analyzers
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
 
 
# ══════════════════════════════════════════════════════
# הרצה לבדיקה
# ══════════════════════════════════════════════════════
 
if __name__ == "__main__":
    print("=== בדיקת מודול עיבוד תמונה ===")
    print("Q = יציאה | E = דמות אות GPS חירום")
 
    analyzer = IntersectionAnalyzer(
        intersection_id=1,
        camera_source=0,         # שנה ל-"video.mp4" לבדיקה עם קובץ
        sample_interval_sec=1.0,
        server_url="http://127.0.0.1:8000/state",
    )
 
    while True:
        ret, frame = analyzer.cap.read()
        if not ret:
            break
 
        state = analyzer.analyze_frame()
        if state:
            print(f"\n[{time.strftime('%H:%M:%S')}] צומת {state.intersection_id}")
            for d, lane in state.lanes.items():
                print(f"  {d}: {lane.vehicle_count} רכב | "
                      f"{lane.pedestrian_count} הולכי רגל | "
                      f"עומס {lane.density_pct}% | "
                      f"המתנה {lane.waiting_time_sec:.0f}s")
            print(f"  RL vector ({len(state.to_rl_vector())} ערכים): {state.to_rl_vector()}")
 
            vis = analyzer.visualize(frame, state)
            cv2.imshow("Intersection Analyzer", vis)
 
        key = cv2.waitKey(1) & 0xFF
        if key == ord('q'):
            break
        elif key == ord('e'):
            # סימולציה של קבלת אות GPS חירום מהבקר ב-C++
            analyzer.set_emergency_signal(GPSEmergencySignal(
                active=True,
                lane_direction="N",
                vehicle_id="AMBULANCE_001",
                timestamp=time.time(),
            ))
 
    analyzer.release()
    cv2.destroyAllWindows()