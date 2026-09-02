"""
תפקיד הקובץ במערכת:

  זהו מודול שכבת הראייה (Vision Layer) של המערכת.
  הוא אחראי על כל מה שקשור לזיהוי תנועה מקל הווידאו:
    1. פתיחת מצלמה/קובץ וידאו
    2. הרצת מודל YOLO לזיהוי רכבים, אוטובוסים, אנשים וכו'
    3. ספירת הישויות בכל אזור/נתיב בצומת
    4. חישוב צפיפות וזמן המתנה לכל נתיב
    5. בנית IntersectionState ושליחתו לשרת HTTP

מי קורא לקובץ הזה?

   python/auto_launcher.py — הקובץ הראשי שמפעיל את המערכת.
    הוא מייבא: IntersectionAnalyzer, IntersectionState,
               GPSEmergencySignal, sign_emergency_signal
    ויוצר אובייקט IntersectionAnalyzer לכל צומת בנפרד ב-thread נפרד.
   python/vision/__init__.py — מייצא את הקלאסים לשאר חלקי הפקגג'
   הרצה ישירה (__main__) — לצורך debug ויזואלי עצמאי

לאן הנתונים נשלחים?

  post_state_to_server() שולחת POST בפורמט JSON לכתובת:
    http://127.0.0.1:8000/intersection/{id}/state
  (ניתן לשנות דרך משתנה סביבה STATE_ENDPOINT)

  השרת המקבל הוא: cpp/server/TrafficServer.cpp
  שמחשב פעולה (Phase0/Phase1) ומחזיר תשובה.

זרימת הנתונים:

  מצלמה/וידאו
      ↓  (OpenCV)
  פריים גולמי
      ↓  (YOLO yolov8n.pt)
  רשימת זיהויים (bounding boxes)
      ↓  (חלוקה לאזורי נתיב)
  LaneState × N  (vehicle_count, density_pct, waiting_time_sec)
      ↓
  IntersectionState (JSON)
      ↓  (HTTP POST)
  TrafficServer.cpp → Phase0 / Phase1

מה המודול הזה עושה:
     ספירת ישויות תנועה בכל נתיב (לפי מספר המצלמות) — YOLO
     כל אזור תנועה נחשב כנתיב רגיל (ללא הפרדת הולכי רגל)
     RL vector דינאמי עם ספירות מדויקות בלבד        — to_rl_vector()

קלט:  מספר נתיבים, פריים וידאו מהמצלמה
פלט:  IntersectionState — וקטור מצב מלא שנכנס לאלגוריתם RL

תלויות:
    pip install ultralytics opencv-python numpy
"""

import cv2                                    # קריאת פריימים מהמצלמה/וידאו וציור על גבי הפריים
import numpy as np                            # עיבוד מערכים מספריים (פריימים = מערכי numpy)
import os                                     # קריאת משתני סביבה (STATE_ENDPOINT)
import time                                   # חישוב זמנים וזמני המתנה
import json                                   # סריאליזציה של IntersectionState לפורמט JSON
import hmac                                   # חתימת HMAC-SHA256 לאות חירום
import hashlib                                # אלגוריתם SHA256 לחתימה
import urllib.request                         # שליחת HTTP POST לשרת C++
import urllib.error                           # טיפול בשגיאות חיבור
from dataclasses import dataclass, asdict, field  # בניית מבני נתונים נקיים
from typing import Optional, List            # טיפוסים עבור type hints


# 
# מבני נתונים
# 

@dataclass
class GPSEmergencySignal:
    """
    אות חירום המגיע מלחצן ה-GPS ברכב החירום.

    מי יוצר אובייקט זה:
       auto_launcher.py — כשמקבל אות GPIO/GPS מרכב חירום
       IntersectionAnalyzer.set_emergency_signal() — לשמירה על האנליזר
       __main__ debug — לחיצת E להדמיית חירום
    מי מקבל אובייקט זה:
       IntersectionState.emergency_signal — נשלח לשרת C++ ב-JSON
       TrafficServer.cpp → validate_emergency_signal() — בדיקת HMAC
    """
    active: bool                 # האם אות החירום פעיל כרגע
    lane_id: int = 0             # מזהה הנתיב בו נמצא רכב החירום (0, 1, 2, ...)
    vehicle_id: str = ""         # מזהה הרכב (למשל "AMB001") — משמש לבדיקת מפתח HMAC
    timestamp: float = 0.0       # חותמת זמן Unix — לבדיקת clock skew ומניעת replay attacks
    signature: str = ""          # חתימת HMAC-SHA256 — מאמתת שהאות לא זויף


def sign_emergency_signal(signal: GPSEmergencySignal, secret_key: str) -> GPSEmergencySignal:
    """
    חותם אות חירום באמצעות HMAC-SHA256.

    תפקיד: מבטיחה שהשרת C++ יוכל לאמת שהאות הגיע ממקור מורשה.
    קוראים לה: auto_launcher.py לפני שליחת GPSEmergencySignal לשרת.
    הפורמט חייב להתאים לצד השרת (TrafficServer.cpp → validate_emergency_signal):
      {vehicle_id}|{lane_id}|{timestamp:.3f}
    """
    if signal.timestamp <= 0:  # אם לא הוגדרה חותמת זמן — מגדירה את הזמן הנוכחי
        signal.timestamp = time.time()

    payload = f"{signal.vehicle_id}|{signal.lane_id}|{signal.timestamp:.3f}"  # בונה מחרוזת payload בפורמט המוסכם עם השרת
    signal.signature = hmac.new(
        secret_key.encode("utf-8"),   # מפתח הסוד (מוגדר ב-emergency_keys.json בשרת)
        payload.encode("utf-8"),       # הנתונים שנחתמים
        hashlib.sha256,               # אלגוריתם SHA256
    ).hexdigest()                     # ממיר את החתימה למחרוזת hex
    return signal  # מחזירה את האות עם החתימה המעודכנת


@dataclass
class LaneState:
    """
    מצב נתיב יחיד — נוצר עבור כל נתיב בצומת בכל דגימת YOLO.
    נשמר בתוך IntersectionState.lanes ונשלח לשרת C++ כ-JSON.
    """
    lane_id: int                  # מזהה הנתיב (0, 1, 2, ...)
    vehicle_count: int            # מספר הרכבים שזוהו ב-ROI של הנתיב בדגימה האחרונה
    density_pct: float = 0.0      # אחוז שטח ה-ROI שמכוסה על-ידי tounding boxes של רכבים (0–100)
    waiting_time_sec: float = 0.0 # זמן מצטבר (שניות) שעבר מאז שהנתיב ריק לחלוטין


@dataclass
class IntersectionState:
    """
    המצב המלא של צומת עם תמיכה בנתיבים דינאמיים.

    תפקיד: מחזיק את כל הנתונים של צומת בנקודת זמן מסוימת.
    נוצר על-ידי: IntersectionAnalyzer._process_frame()
    נשלח על-ידי: post_state_to_server() → TrafficServer.cpp
    מוצג על-ידי: IntersectionAnalyzer.visualize() על גבי הפריים
    """
    intersection_id:int                                        # מזהה הצומת (1, 2, 3, 4)
    num_lanes: int                                             # מספר הנתיבים בצומת
    timestamp: float                                           # חותמת זמן Unix של הדגימה
    lanes: List[LaneState]                                     # מצב כל נתיב — רשימה באורך num_lanes
    emergency_signal: Optional[GPSEmergencySignal] = None      # אות חירום פעיל (או None)
    neighbor_states: dict = field(default_factory=dict)        # שמור לשימוש עתידי — מצבי צמתים שכנים

    @property
    def total_vehicles(self) -> int:  # מחשב סך כל הרכבים בכל הנתיבים — משמש לכותרת ב-visualize()
        return sum(lane.vehicle_count for lane in self.lanes)

    def to_rl_vector(self) -> list:
        """
        ממיר את המצב לוקטור מספרי בשביל של ה-RL.
        קוראים לה: RLAgent (C++) בעקיפין — דרך הנתונים שנשלחים לשרת.
        וקטור דינאמי: num_lanes ערכים (ספירה מדויקת לכל נתיב).
        """
        return [int(lane.vehicle_count) for lane in self.lanes]  # [רכבים בנתיב0, רכבים בנתיב1, ...]

    def to_json(self) -> str:
        """
        ממיר ל-JSON לשליחה לשרת C++.
        קוראים לה: post_state_to_server() לפני שליחת ה-HTTP POST.
        הפורמט חייב להתאים ל-TrafficServer.cpp שמפרסר את ה-JSON הזה.
        """
        data = {
            "intersection_id": self.intersection_id,  # מזהה הצומת
            "num_lanes": self.num_lanes,               # מספר הנתיבים
            "timestamp": self.timestamp,               # חותמת זמן
            "lanes": [asdict(lane) for lane in self.lanes],  # ממיר כל LaneState ל-dict
            "emergency_signal": (
                asdict(self.emergency_signal) if self.emergency_signal else None  # אות חירום או null
            ),
            "total_vehicles": self.total_vehicles,     # סך הרכבים (שדה נוחות)
        }
        return json.dumps(data, ensure_ascii=False)  # ממיר ל-JSON string עם תמיכה בעברית


def post_state_to_server(state: IntersectionState, server_url: Optional[str] = None) -> Optional[str]:
    """
    שולחת את מצב הצומת לשרת C++ דרך HTTP POST.

    תפקיד: גשר בין שכבת הראייה (Python/YOLO) לשכבת הבקרה (C++ TrafficServer).
    קוראים לה: IntersectionAnalyzer.analyze_frame() אחרי כל דגימת YOLO.
    שולחת ל: http://127.0.0.1:8000/intersection/{id}/state (ברירת מחדל)
              ניתן לשנות דרך משתנה סביבה STATE_ENDPOINT.
    מקבל: TrafficServer.cpp → endpoint POST /intersection/{id}/state
           שמחשב Phase0/Phase1 ומחזיר JSON עם הפעולה.
    """
    if server_url is None:
        server_url = os.environ.get("STATE_ENDPOINT", "http://127.0.0.1:8000/state")  # קריאת כתובת השרת ממשתנה סביבה
    try:
        data = state.to_json().encode("utf-8")  # ממיר את המצב ל-bytes לשליחה
        request = urllib.request.Request(
            server_url,                                      # כתובת השרת
            data=data,                                       # גוף הבקשה — JSON bytes
            headers={"Content-Type": "application/json"},   # כותרת לשרת שיידע לפרסר JSON
            method="POST",                                   # שיטת HTTP
        )
        with urllib.request.urlopen(request, timeout=5) as response:  # שולח ומחכה עד 5 שניות
            return response.read().decode("utf-8")  # מחזיר את תשובת השרת (Phase0/Phase1)
    except urllib.error.URLError as e:
        print(f"[Intersection {state.intersection_id}] שגיאת חיבור: {e}")  # שרת לא פועל או לא נגיש
        return None
    except Exception as e:
        print(f"[Intersection {state.intersection_id}] שגיאת שליחה: {e}")  # שגיאה כללית
        return None

# בניית אזורי נתיבים דינאמיים

def build_lane_zones(frame_width: int, frame_height: int, num_lanes: int) -> List[tuple]:
    """
    בונה רשימה של ROI (Region Of Interest) לכל נתיב בצומת.

    תפקיד: קובעת את הגבולות הפיזיים (בפיקסלים) של כל נתיב על גבי הפריים.
            כל ROI הוא tuple: (x1, y1, x2, y2) — פינה שמאלית-עליונה ופינה ימנית-תחתונה.
    קוראים לה: IntersectionAnalyzer.__init__() פעם אחת בלבד בהפעלה.
    מאפשרת צמתים עם מספר משתנה של נתיבים (2, 3, 4, או כל מספר אחר).
    """
    w, h = frame_width, frame_height  # ממדי הפריים בפיקסלים

    if num_lanes == 4:  # צומת 4-כיוונית — נפוצה ביותר
        cx, cy = w // 2, h // 2  # מרכז הפריים
        return [
            (cx - 100, 0, cx + 100, cy - 20),           # lane 0: North — גזרה צפונית (עליונה)
            (cx - 100, cy + 20, cx + 100, h),           # lane 1: South — גזרה דרומית (תחתונה)
            (0, cy - 100, cx - 20, cy + 100),           # lane 2: West  — גזרה מערבית (שמאל)
            (cx + 20, cy - 100, w, cy + 100),           # lane 3: East  — גזרה מזרחית (ימין)
        ]
    elif num_lanes == 3:  # צומת T — שלושה כיוונים
        cx, cy = w // 2, h // 2  # מרכז הפריים
        return [
            (cx - 100, 0, cx + 100, cy - 20),           # lane 0: Top          — ראש ה-T
            (0, cy - 50, cx - 20, h),                   # lane 1: Bottom-Left  — שמאל תחתון
            (cx + 20, cy - 50, w, h),                   # lane 2: Bottom-Right — ימין תחתון
        ]
    elif num_lanes == 2:  # כביש דו-כיווני פשוט
        cx, cy = w // 2, h // 2  # מרכז הפריים
        return [
            (cx - 100, 0, cx + 100, cy - 20),           # lane 0: Top    — כיוון עליון
            (cx - 100, cy + 20, cx + 100, h),           # lane 1: Bottom — כיוון תחתון
        ]
    else:
        # חלוקה ליניארית לכל מספר אחר — מחלקת את הפריים לפסים אופקיים שווים
        zones = []
        lane_height = h // num_lanes  # גובה כל פס בפיקסלים
        for i in range(num_lanes):
            y1 = i * lane_height        # גבול עליון של הנתיב
            y2 = (i + 1) * lane_height  # גבול תחתון של הנתיב
            zones.append((0, y1, w, y2))  # מוסיף ROI שמכסה את כל רוחב הפריים
        return zones

# YOLO — זיהוי ישויות תנועה

# ── מזהי מחלקות YOLO שנספרים כ"תנועה" ─────────────────────────────────────
# YOLO מחזיר מזהה מחלקה (class_id) לכל אובייקט שמזהה.
# אנחנו סופרים רק את המחלקות האלה — person=0, car=2, motorcycle=3, bus=5, truck=7
# כל השאר (אופניים, כלבים, עצים וכו') מתעלמים.
TRAFFIC_CLASS_IDS = {0, 2, 3, 5, 7}    # person, car, motorcycle, bus, truck

# סוכן צומת ראשי

class IntersectionAnalyzer:
    """
    הקלאס המרכזי של המודול — סוכן עצמאי לניתוח תנועה בצומת אחת.

    תפקיד: מנהלת את כל מחזור החיים של ניתוח צומת:
            פתיחת מצלמה → הרצת YOLO → ספירה לפי נתיב → שליחה לשרת
    מי יוצר אותה:
       auto_launcher.py — יוצר IntersectionAnalyzer אחד לכל צומת ב-thread נפרד
       __main__ debug — יוצר מופע אחד להדמיה ויזואלית
    מי קורא לה:
       auto_launcher.py קורא ל-analyze_frame() בלולאה
       auto_launcher.py קורא ל-set_emergency_signal() כשמגיע GPS
    """

    def __init__(
        self,
        intersection_id: int,      # מזהה הצומת (1–4)
        num_lanes: int,            # מספר הנתיבים (2, 3 או 4)
        camera_source,             # מספר מצלמה (0,1,...) או נתיב לקובץ וידאו
        model_path: str = "yolov8n.pt",     # נתיב למשקלי YOLO (ברירת מחדל: nano)
        confidence: float = 0.45,           # סף ביטחון YOLO — זיהויים מתחת לסף מתעלמים
        sample_interval_sec: float = 2.0,   # כל כמה שניות מריצים YOLO (חיסכון ב-CPU)
        server_url: Optional[str] = None,   # כתובת שרת לשליחה — None = קריאה ממשתנה סביבה
    ):
        self.intersection_id = intersection_id  # שמירת מזהה הצומת
        self.num_lanes = num_lanes              # שמירת מספר הנתיבים
        self.confidence = confidence            # שמירת סף הביטחון
        self.sample_interval = sample_interval_sec  # שמירת מרווח הדגימה
        self._last_sample_time = 0.0            # זמן הדגימה האחרונה — מאופס בהתחלה
        self.server_url = server_url            # שמירת כתובת השרת
        self._emergency_signal: Optional[GPSEmergencySignal] = None  # אות חירום נוכחי (None = אין)
        self._waiting_times = [0.0] * num_lanes  # מונה זמן המתנה מצטבר לכל נתיב
        self.last_frame: Optional[np.ndarray] = None   # הפריים האחרון שנקרא מהמצלמה
        self.last_detections: list = []                # תוצאות YOLO האחרונות — לויזואליזציה

        from ultralytics import YOLO  # ייבוא מאוחר — נמנע מטעינת YOLO אם המודול לא בשימוש
        print(f"[Intersection {intersection_id}] טוען YOLO...")
        self.model = YOLO(model_path)  # טוענת את מודל YOLO עם המשקלים

        self.cap = cv2.VideoCapture(camera_source)  # פותחת את המצלמה/קובץ וידאו
        if not self.cap.isOpened():
            raise RuntimeError(f"לא ניתן לפתוח מקור וידאו: {camera_source}")  # מקור לא תקין

        ret, frame = self.cap.read()  # קוראת פריים ראשון לבדיקה ולקבלת ממדים
        if not ret:
            raise RuntimeError("לא ניתן לקרוא פריים ראשון")  # המצלמה לא מחזירה תמונה

        h, w = frame.shape[:2]  # מחלצת גובה ורוחב הפריים
        self.lane_zones = build_lane_zones(w, h, num_lanes)  # בונה אזורי ROI לפי ממדי הפריים
        print(f"[Intersection {intersection_id}] מוכן | {w}x{h} | {num_lanes} נתיבים")

    def set_emergency_signal(self, signal: GPSEmergencySignal):
        """
        שומרת אות חירום GPS שיצורף לדגימה הבאה ויישלח לשרת C++.
        קוראים לה: auto_launcher.py כשמגיע אות GPIO/GPS מרכב חירום.
        האות נשמר עד שקוראים ל-clear_emergency_signal().
        """
        self._emergency_signal = signal  # שמירת האות להוספה ל-IntersectionState הבא
        if signal.active:
            print(f"[Intersection {self.intersection_id}] GPS EMERGENCY! lane {signal.lane_id}")

    def clear_emergency_signal(self):
        """
        מוחקת את אות החירום הנוכחי.
        קוראים לה: auto_launcher.py לאחר שהשרת אישר קבלת אות החירום.
        """
        self._emergency_signal = None  # מאפסת את האות — הדגימות הבאות ישלחו ללא אות חירום

    def analyze_frame(self) -> Optional[IntersectionState]:
        """
        הפונקציה הראשית שנקראת בלולאה — קוראת פריים ומריצה YOLO אם הגיע הזמן.

        תפקיד: מנהלת throttling — YOLO לא רץ על כל פריים (יקר מדי),
                אלא רק כל sample_interval_sec שניות.
        קוראים לה: auto_launcher.py בלולאה רציפה (כ-30 פעמים בשניה)
        מחזירה: IntersectionState אם בוצעה דגימה, None אחרת (פריים ביניים)
        """
        ret, frame = self.cap.read()  # קוראת פריים חדש מהמצלמה/וידאו
        if not ret:
            return None  # סוף וידאו או שגיאת מצלמה
        self.last_frame = frame  # שומרת את הפריים תמיד — גם בין דגימות YOLO (לויזואליזציה)

        now = time.time()  # קוראת את הזמן הנוכחי
        if now - self._last_sample_time < self.sample_interval:
            return None  # עדיין לא הגיע זמן דגימה — דולגת על הרצת YOLO

        self._last_sample_time = now  # מעדכנת את זמן הדגימה האחרונה
        state = self._process_frame(frame, now)  # מריצה YOLO ובונה IntersectionState

        if self.server_url and state is not None:
            post_state_to_server(state, self.server_url)  # שולחת את המצב לשרת C++

        return state  # מחזירה את המצב לקורא (auto_launcher / __main__)

    def _process_frame(self, frame: np.ndarray, timestamp: float) -> IntersectionState:
        """
        מעבד פריים וידאו וממיר אותו ל-IntersectionState — הפונקציה הפנימית המרכזית.

        זרימה:
          1. מריצה YOLO על הפריים → רשימת bounding boxes
          2. לכל נתיב — סופרת כמה מרכזי bounding boxes נמצאים ב-ROI שלו
          3. מחשבת צפיפות (density_pct) וזמן המתנה (waiting_time_sec)
          4. בונה רשימת LaneState ואורזת ב-IntersectionState
        קוראים לה: analyze_frame() כשמגיע זמן דגימה
        """
        results = self.model(frame, conf=self.confidence, verbose=False)[0]  # הרצת YOLO — מחזיר תוצאות זיהוי
        detections = self._parse_detections(results)  # ממירה תוצאות YOLO לרשימת dicts נוחה
        self.last_detections = detections  # שמירה לויזואליזציה חיצונית

        lanes = []  # תבנה רשימת LaneState לכל נתיב
        for lane_id in range(self.num_lanes):  # עוברת על כל הנתיבים
            x1, y1, x2, y2 = self.lane_zones[lane_id]  # גבולות ה-ROI של הנתיב
            x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)  # ממירה לפיקסלים שלמים

            vehicles = 0  # מונה רכבים בנתיב זה
            for det in detections:  # עוברת על כל הזיהויים
                cx, cy = det["cx"], det["cy"]  # מרכז ה-bounding box
                if x1 <= cx <= x2 and y1 <= cy <= y2:  # מרכז ה-box בתוך ה-ROI?
                    vehicles += 1  # כן — סופרת אותו לנתיב זה

            density_pct = self._compute_lane_density_pct(detections, x1, y1, x2, y2)  # מחשבת צפיפות לפי שטח

            if vehicles > 0:  # אם יש רכבים בנתיב
                self._waiting_times[lane_id] += self.sample_interval  # מצטברת זמן המתנה
            else:
                self._waiting_times[lane_id] = 0.0  # אין רכבים — מאפסת את המונה

            lanes.append(LaneState(
                lane_id=lane_id,
                vehicle_count=vehicles,           # ספירת רכבים מדויקת
                density_pct=density_pct,          # אחוז כיסוי ה-ROI
                waiting_time_sec=self._waiting_times[lane_id],  # זמן המתנה מצטבר
            ))

        return IntersectionState(  # בונה ומחזירה את המצב המלא של הצומת
            intersection_id=self.intersection_id,
            num_lanes=self.num_lanes,
            timestamp=timestamp,
            lanes=lanes,
            emergency_signal=self._emergency_signal,  # מוסיפה אות חירום אם קיים
        )

    def _parse_detections(self, results) -> list:
        """
        ממירה תוצאות YOLO גולמיות לרשימת dicts פשוטה.

        תפקיד: מסנן רק את המחלקות שמוגדרות ב-TRAFFIC_CLASS_IDS ומחשב מרכז כל box.
        קוראים לה: _process_frame() אחרי הרצת YOLO.
        מחזירה: רשימת dicts עם מפתחות: class_id, x1, y1, x2, y2, cx, cy
        """
        detections = []  # רשימת הזיהויים שיוחזרו
        if results.boxes is None:
            return detections  # YOLO לא מצא שום דבר — מחזיר רשימה ריקה
        for box in results.boxes:  # עוברת על כל הזיהויים שהחזיר YOLO
            class_id = int(box.cls[0])  # מחלצת את מזהה המחלקה
            if class_id not in TRAFFIC_CLASS_IDS:  # מסננת מחלקות לא רלוונטיות
                continue
            x1, y1, x2, y2 = box.xyxy[0].tolist()  # מחלצת קואורדינטות ה-bounding box
            detections.append({
                "class_id": class_id,      # מזהה המחלקה (רכב/אדם/אוטובוס)
                "x1": x1,                 # פינה שמאלית-עליונה X
                "y1": y1,                 # פינה שמאלית-עליונה Y
                "x2": x2,                 # פינה ימנית-תחתונה X
                "y2": y2,                 # פינה ימנית-תחתונה Y
                "cx": (x1 + x2) / 2,      # מרכז X — משמש לבדיקה אם בתוך ROI
                "cy": (y1 + y2) / 2,      # מרכז Y — משמש לבדיקה אם בתוך ROI
            })
        return detections  # רשימת כל הזיהויים הרלוונטיים

    def _compute_lane_density_pct(self, detections: list, x1: int, y1: int, x2: int, y2: int) -> float:
        """
        מחשבת אחוז הצפיפות בנתיב — כמה אחוז משטח ה-ROI מכוסה על-ידי רכבים.

        שיטה: חישוב intersection (חיתוך) של כל bounding box עם ה-ROI,
              סכימת השטחים, וחלוקה בשטח ה-ROI הכולל.
        קוראים לה: _process_frame() לכל נתיב אחרי ספירת הרכבים.
        מחזירה: ערך בין 0.0 ל-100.0 (אחוז)
        """
        roi_w = max(0, x2 - x1)  # רוחב ה-ROI בפיקסלים
        roi_h = max(0, y2 - y1)  # גובה ה-ROI בפיקסלים
        roi_area = roi_w * roi_h  # שטח ה-ROI הכולל
        if roi_area == 0:
            return 0.0  # ROI חסר גודל — מחזירה 0 למניעת חלוקה ב-0

        occupied_area = 0.0  # סכום שטח החיתוך של כל הרכבים עם ה-ROI
        for det in detections:  # עוברת על כל הזיהויים
            ix1 = max(x1, int(det["x1"]))  # גבול שמאלי של החיתוך
            iy1 = max(y1, int(det["y1"]))  # גבול עליון של החיתוך
            ix2 = min(x2, int(det["x2"]))  # גבול ימני של החיתוך
            iy2 = min(y2, int(det["y2"]))  # גבול תחתון של החיתוך
            iw = max(0, ix2 - ix1)          # רוחב החיתוך (0 אם אין חיתוך)
            ih = max(0, iy2 - iy1)          # גובה החיתוך (0 אם אין חיתוך)
            occupied_area += iw * ih        # מוסיפה את שטח החיתוך

        density = min((occupied_area / roi_area) * 100.0, 100.0)  # ממירה לאחוזים, מגבילה ל-100
        return round(density, 1)  # מחזירה עם דיוק של ספרה אחת אחרי הנקודה

    def visualize(self, frame: np.ndarray, state: IntersectionState,
                  detections: list | None = None) -> np.ndarray:
        """
        מצייר מידע debug על גבי הפריים — לתצוגה ויזואלית בלבד, לא נשלח לשרת.

        מה מצויר:
           bounding boxes צהובים של YOLO עם תווית מחלקה (אם detections סופק)
           מלבן צבעוני לכל נתיב עם: מספר רכבים, צפיפות, זמן המתנה
           כותרת תחתונה עם סך הרכבים
           פס אדום בחלק העליון עם הודעת חירום (אם פעיל)
        קוראים לה: __main__ debug loop לכל פריים שמוצג על המסך
        מחזירה: פריים חדש עם כל הציורים (לא משנה את הפריים המקורי)
        """
        vis = frame.copy()  # עובדת על עותק — לא משנה את הפריים המקורי
        colors = [(0, 220, 0), (255, 120, 0), (0, 120, 255), (220, 220, 0),
                  (180, 0, 220), (0, 200, 200)]  # צבע שונה לכל נתיב

        # ── bounding boxes של YOLO (אם סופקו) ─────────────────────────────
        CLASS_NAMES = {0: "person", 2: "car", 3: "moto", 5: "bus", 7: "truck"}  # שמות מחלקות לתווית
        if detections:  # אם הועברה רשימת זיהויים
            for det in detections:  # עוברת על כל זיהוי
                bx1, by1 = int(det["x1"]), int(det["y1"])  # פינה שמאלית-עליונה
                bx2, by2 = int(det["x2"]), int(det["y2"])  # פינה ימנית-תחתונה
                label = CLASS_NAMES.get(det["class_id"], "?")  # שם המחלקה
                cv2.rectangle(vis, (bx1, by1), (bx2, by2), (0, 255, 255), 2)  # מלבן צהוב-ירוק
                cv2.putText(vis, label, (bx1, by1 - 4),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 255, 255), 1)  # תווית מחלקה

        # ── גבולות נתיבים וסטטיסטיקה ──────────────────────────────────────
        for lane_id, (x1, y1, x2, y2) in enumerate(self.lane_zones):  # עוברת על כל הנתיבים
            x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)  # ממירה לפיקסלים שלמים
            lane = state.lanes[lane_id]  # מחלצת נתוני הנתיב מה-state
            color = colors[lane_id % len(colors)]  # בוחרת צבע ייחודי לנתיב
            cv2.rectangle(vis, (x1, y1), (x2, y2), color, 2)  # מצייר מלבן גבולות הנתיב

            lines = [
                f"Lane {lane_id}: Count={lane.vehicle_count}",         # שורה 1: ספירת רכבים
                f"Density: {lane.density_pct:.1f}% | Wait: {lane.waiting_time_sec:.0f}s",  # שורה 2: צפיפות וזמן המתנה
            ]
            for i, text in enumerate(lines):  # מצייר כל שורת טקסט
                cv2.putText(vis, text, (x1 + 5, y1 + 20 + i * 20),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 2)

        # ── כותרת כללית ───────────────
        total = sum(l.vehicle_count for l in state.lanes)  # סך כל הרכבים בצומת
        cv2.putText(vis, f"Intersection {state.intersection_id} | Total: {total} vehicles",
                    (10, vis.shape[0] - 10),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)  # טקסט לבן בתחתית

        if state.emergency_signal and state.emergency_signal.active:  # אם יש אות חירום פעיל
            msg = f"GPS EMERGENCY -- Lane {state.emergency_signal.lane_id}"
            cv2.rectangle(vis, (0, 0), (vis.shape[1], 50), (0, 0, 200), -1)  # פס אדום בחלק עליון
            cv2.putText(vis, msg, (10, 35),
                        cv2.FONT_HERSHEY_SIMPLEX, 1.0, (255, 255, 255), 2)  # הודעת חירום בפס

        return vis  # מחזירה את הפריים עם כל הציורים

    def release(self):
        """משחררת את אחיזת OpenCV על המצלמה/קובץ הוידאו — חשוב לקרוא בסיום."""
        self.cap.release()  # משחררת את ה-VideoCapture של OpenCV

    def __del__(self):
        """destructor — נקרא אוטומטית כשהאובייקט נהרס, מבטיח שחרור המצלמה."""
        self.release()  # מבטיח שהמצלמה תשוחרר גם אם שכחו לקרוא ל-release() ידנית

# בדיקה

# הרצה עצמאית לצורך debug ויזואלי
# הפעל: python intersection_vision.py --video video.mp4 --lanes 4
# לחץ Q לסגירה, E להדמיית אות חירום

if __name__ == "__main__":
    import argparse as _ap
    _parser = _ap.ArgumentParser(description="Vision debug viewer")  # מגדיר ממשק שורת פקודה
    _parser.add_argument("--video", default=None, metavar="PATH",
                         help="Video file path (default: webcam 0)")  # קובץ וידאו או מצלמה
    _parser.add_argument("--lanes", type=int, default=4,
                         help="Number of lanes (default: 4)")  # מספר נתיבים לבדיקה
    _args = _parser.parse_args()

    camera_source = _args.video if _args.video else 0  # קובץ וידאו אם הועבר, אחרת מצלמה 0
    print("=== Vision Module Debug Viewer ===")
    print(f"Source: {camera_source} | Lanes: {_args.lanes}")
    print("Q = exit | E = simulate emergency")

    analyzer = IntersectionAnalyzer(
        intersection_id=1,                   # צומת מספר 1 לצורך הבדיקה
        num_lanes=_args.lanes,               # מספר נתיבים מהפרמטר
        camera_source=camera_source,         # מקור הוידאו
        sample_interval_sec=0.5,             # עדכון מהיר (0.5 שניות) לצורך ויזואליזציה חלקה
        server_url=os.environ.get("STATE_ENDPOINT", "http://127.0.0.1:8000/state"),  # כתובת שרת
    )

    last_state = None        # המצב האחרון שחושב — לציור על פריימים ביניים
    last_detections: list = []  # הזיהויים האחרונים של YOLO — לציור bounding boxes

    while True:  # לולאה ראשית — מעבדת פריים פריים
        ret, frame = analyzer.cap.read()  # קוראת פריים ישירות (לא דרך analyze_frame)
        if not ret:
            # סוף וידאו — חזור להתחלה כדי שהויזואליזציה תמשיך
            analyzer.cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            continue

        # מריץ YOLO על כל פריים (ללא throttle) לצורך ויזואליזציה חלקה
        results = analyzer.model(frame, conf=analyzer.confidence, verbose=False)[0]
        last_detections = analyzer._parse_detections(results)  # מחלץ זיהויים לציור

        state = analyzer.analyze_frame()  # בודק אם הגיע זמן דגימה ושולח לשרת
        if state:  # אם הגיע זמן דגימה — מעדכן
            last_state = state
            print(f"[{time.strftime('%H:%M:%S')}] "  # שעה נוכחית
                  + " | ".join(f"L{l.lane_id}:{l.vehicle_count}" for l in state.lanes)  # ספירות לכל נתיב
                  + f" | total={state.total_vehicles}")  # סך הכל

        if last_state:  # מציג ויזואליזציה אם יש מצב זמין
            vis = analyzer.visualize(frame, last_state, detections=last_detections)  # מצייר debug
            cv2.imshow("YOLO Detection Debug", vis)  # מציג בחלון

        key = cv2.waitKey(1) & 0xFF  # בודק קלט מקלדת (ממתין 1ms)
        if key in (ord('q'), ord('Q')):  # Q — יציאה
            break
        elif key == ord('e'):  # E — הדמיית אות חירום לבדיקה
            analyzer.set_emergency_signal(GPSEmergencySignal(
                active=True, lane_id=1,
                vehicle_id="AMBULANCE_001", timestamp=time.time(),  # רכב חירום מזויף לבדיקה
            ))

    analyzer.release()        # משחררת את המצלמה
    cv2.destroyAllWindows()   # סוגרת את כל חלונות OpenCV
