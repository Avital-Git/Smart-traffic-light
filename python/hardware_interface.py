"""
hardware_interface.py
─────────────────────
שכבת ממשק חומרה (Hardware Abstraction Layer)
אביטל חדד | מכללת בנות בת שבע

עיקרון הקובץ:
  כל חיישן — מצלמה, לחצן GPS, חיישן LED — מוגדר כ-interface (מחלקת בסיס).
  יש שתי מימושים לכל interface:
    1. Mock (סימולציה)  ← עובד עכשיו בלי חומרה
    2. Real             ← יתחבר לחומרה אמיתית כשתגיע

  כדי לעבור מסימולציה לחומרה אמיתית:
    שנה בלבד את USE_REAL_HARDWARE =ית הקובץ.
"""

from __future__ import annotations
import random
import time
import threading
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Optional, Callable


# ══════════════════════════════════════════════════════════════
# מבני נתונים משותפים
# ══════════════════════════════════════════════════════════════

@dataclass
class CameraFrame:
    """פריים יחיד ממצלמה."""
    intersection_id: int
    lane_id: int          # 0=N, 1=S, 2=E, 3=W
    timestamp: float
    vehicle_count: int
    density_pct: float    # 0–100
    pedestrian_count: int = 0
    raw_frame: object = None   # numpy array כשיש מצלמה אמיתית


@dataclass
class GPSEmergencyEvent:
    """אירוע לחיצה על לחצן GPS ברכב חירום."""
    active: bool
    lane_id: int          # הנתיב ממנו הגיע האות (0–3)
    vehicle_id: str       # מזהה הרכב (לחתימה דיגיטלית)
    timestamp: float = field(default_factory=time.time)

    @property
    def direction(self) -> str:
        return ["N", "S", "E", "W"][self.lane_id % 4]


@dataclass
class LEDSensorReading:
    """קריאת חיישן LED — זיהוי נוכחות רכב בנתיב."""
    lane_id: int
    vehicle_present: bool
    timestamp: float = field(default_factory=time.time)


# ══════════════════════════════════════════════════════════════
# ממשקי חומרה (Abstract Base Classes)
# ══════════════════════════════════════════════════════════════

class CameraInterface(ABC):
    """ממשק מצלמה — מחזיר פריים עם נתוני תנועה."""

    @abstractmethod
    def read_frame(self, lane_id: int) -> Optional[CameraFrame]:
        """קריאת פריים מנתיב מסוים. מחזיר None אם אין פריים זמין."""

    @abstractmethod
    def is_connected(self) -> bool:
        """האם המצלמה מחוברת ועובדת."""

    @abstractmethod
    def release(self):
        """שחרור משאבי המצלמה."""


class GPSButtonInterface(ABC):
    """ממשק לחצן GPS לרכבי חירום."""

    @abstractmethod
    def listen(self, callback: Callable[[GPSEmergencyEvent], None]):
        """
        מאזין לאות GPS.
        callback נקרא בכל פעם שרכב חירום לוחץ על הלחצן.
        """

    @abstractmethod
    def stop(self):
        """עצירת ההאזנה."""


class LEDSensorInterface(ABC):
    """ממשק חיישן LED לזיהוי נוכחות רכב."""

    @abstractmethod
    def read(self, lane_id: int) -> LEDSensorReading:
        """קריאת מצב חיישן LED עבור נתיב."""

    @abstractmethod
    def is_connected(self) -> bool:
        """האם החיישן מחובר."""


# ══════════════════════════════════════════════════════════════
# מימוש MOCK — סימולציה (ללא חומרה)
# ══════════════════════════════════════════════════════════════

class MockCamera(CameraInterface):
    """
    מצלמה מדומה — מייצרת נתוני תנועה אקראיים.
    כאשר תגיע מצלמה אמיתית — החלף ב-RealCamera.
    """

    TRAFFIC_PATTERNS = {
        # שעה: (min_vehicles, max_vehicles)
        (7, 9):   (4, 12),   # שעות עומס בוקר
        (12, 13): (3, 8),    # צהריים
        (16, 19): (5, 14),   # שעות עומס ערב
        (22, 6):  (0, 2),    # לילה
    }

    def __init__(self, intersection_id: int, num_lanes: int = 4):
        self.intersection_id = intersection_id
        self.num_lanes = num_lanes
        self._connected = True
        print(f"[MockCamera] צומת {intersection_id} — {num_lanes} נתיבים (סימולציה)")

    def _get_realistic_vehicle_count(self) -> int:
        hour = time.localtime().tm_hour
        for (h_start, h_end), (lo, hi) in self.TRAFFIC_PATTERNS.items():
            if h_start <= h_end:
                if h_start <= hour < h_end:
                    return random.randint(lo, hi)
            else:  # עובר חצות
                if hour >= h_start or hour < h_end:
                    return random.randint(lo, hi)
        return random.randint(1, 6)

    def read_frame(self, lane_id: int) -> Optional[CameraFrame]:
        if lane_id >= self.num_lanes:
            return None
        vehicles = self._get_realistic_vehicle_count()
        # תנודתיות קטנה בין נתיבים
        vehicles = max(0, vehicles + random.randint(-2, 2))
        density = min(100.0, round(vehicles * random.uniform(7.0, 11.0), 1))
        pedestrians = random.choices([0, 1, 2], weights=[60, 30, 10])[0]
        return CameraFrame(
            intersection_id=self.intersection_id,
            lane_id=lane_id,
            timestamp=time.time(),
            vehicle_count=vehicles,
            density_pct=density,
            pedestrian_count=pedestrians,
        )

    def is_connected(self) -> bool:
        return self._connected

    def release(self):
        self._connected = False
        print(f"[MockCamera] צומת {self.intersection_id} — שוחרר")


class MockGPSButton(GPSButtonInterface):
    """
    לחצן GPS מדומה — מדמה לחיצות חירום בהסתברות נמוכה.
    כאשר יגיע לחצן GPS אמיתי — החלף ב-RealGPSButton.

    ניתן גם להפעיל אות ידנית: mock_gps.simulate_press(lane_id=0)
    """

    def __init__(self, intersection_id: int, auto_interval_sec: float = 60.0):
        """
        auto_interval_sec: כל כמה שניות לבדוק אם לשלוח אות מדומה
                           (הסתברות נמוכה בכל בדיקה)
        """
        self.intersection_id = intersection_id
        self.auto_interval = auto_interval_sec
        self._callback: Optional[Callable] = None
        self._running = False
        self._thread: Optional[threading.Thread] = None

    def listen(self, callback: Callable[[GPSEmergencyEvent], None]):
        self._callback = callback
        self._running = True
        self._thread = threading.Thread(target=self._auto_simulate, daemon=True)
        self._thread.start()
        print(f"[MockGPS] צומת {self.intersection_id} — מאזין (סימולציה אוטומטית כל {self.auto_interval}s)")

    def _auto_simulate(self):
        """כל auto_interval שניות — 5% סיכוי לאות חירום מדומה."""
        while self._running:
            time.sleep(self.auto_interval)
            if self._running and random.random() < 0.05:
                lane = random.randint(0, 3)
                self.simulate_press(lane_id=lane, vehicle_id="SIM_AMBULANCE_001")

    def simulate_press(self, lane_id: int = 0, vehicle_id: str = "SIM_VEH_001"):
        """קריאה ידנית לדמות לחיצה על לחצן GPS — לשימוש בבדיקות."""
        if self._callback:
            event = GPSEmergencyEvent(
                active=True,
                lane_id=lane_id,
                vehicle_id=vehicle_id,
                timestamp=time.time(),
            )
            print(f"[MockGPS] 🚨 אות חירום מדומה! נתיב {event.direction} | רכב {vehicle_id}")
            self._callback(event)

    def stop(self):
        self._running = False
        print(f"[MockGPS] צומת {self.intersection_id} — הופסק")


class MockLEDSensor(LEDSensorInterface):
    """
    חיישן LED מדומה — מחזיר נוכחות רכב לפי הסתברות.
    כאשר יגיע חיישן LED אמיתי — החלף ב-RealLEDSensor.
    """

    def __init__(self, intersection_id: int):
        self.intersection_id = intersection_id
        print(f"[MockLED] צומת {intersection_id} — חיישן LED מדומה")

    def read(self, lane_id: int) -> LEDSensorReading:
        # 70% סיכוי לנוכחות רכב בשעות פעילות
        hour = time.localtime().tm_hour
        prob = 0.70 if 7 <= hour <= 20 else 0.15
        return LEDSensorReading(
            lane_id=lane_id,
            vehicle_present=random.random() < prob,
            timestamp=time.time(),
        )

    def is_connected(self) -> bool:
        return True


# ══════════════════════════════════════════════════════════════
# מימוש REAL — חומרה אמיתית (מוכן לחיבור עתידי)
# ══════════════════════════════════════════════════════════════

class RealCamera(CameraInterface):
    """
    מצלמה אמיתית דרך OpenCV.
    camera_source: מספר מצלמה (0=USB), או RTSP URL מהצומת.
    עיבוד התמונה מתבצע ב-intersection_vision.py (YOLO + Background Subtraction).
    """

    def __init__(self, intersection_id: int, camera_source, num_lanes: int = 4):
        import cv2
        from vision.intersection_vision import IntersectionAnalyzer
        self.intersection_id = intersection_id
        self.num_lanes = num_lanes
        print(f"[RealCamera] מאתחל מצלמה: {camera_source}")
        self._analyzer = IntersectionAnalyzer(
            intersection_id=intersection_id,
            camera_source=camera_source,
        )
        print(f"[RealCamera] צומת {intersection_id} — מחוברת ✅")

    def read_frame(self, lane_id: int) -> Optional[CameraFrame]:
        state = self._analyzer.analyze_frame()
        if state is None:
            return None
        directions = ["N", "S", "E", "W"]
        if lane_id >= len(directions):
            return None
        direction = directions[lane_id]
        lane = state.lanes.get(direction)
        if lane is None:
            return None
        return CameraFrame(
            intersection_id=self.intersection_id,
            lane_id=lane_id,
            timestamp=state.timestamp,
            vehicle_count=lane.vehicle_count,
            density_pct=lane.density_pct,
            pedestrian_count=lane.pedestrian_count,
        )

    def is_connected(self) -> bool:
        return self._analyzer.cap.isOpened()

    def release(self):
        self._analyzer.release()
        print(f"[RealCamera] צומת {self.intersection_id} — שוחרר")


class RealGPSButton(GPSButtonInterface):
    """
    לחצן GPS אמיתי — מאזין לפורט סריאלי או לשידור UDP.

    כשהרכיב ישדר אות:
      - פורט סריאלי:  RealGPSButton(mode="serial", port="COM3")
      - UDP:           RealGPSButton(mode="udp", port=5005)

    הפרוטוקול: JSON חתום דיגיטלית (HMAC) — לפי ההצעה.
    """

    def __init__(self, intersection_id: int, mode: str = "udp",
                 port=5005, host: str = "0.0.0.0"):
        self.intersection_id = intersection_id
        self.mode = mode
        self.port = port
        self.host = host
        self._callback: Optional[Callable] = None
        self._running = False

    def listen(self, callback: Callable[[GPSEmergencyEvent], None]):
        self._callback = callback
        self._running = True
        if self.mode == "udp":
            t = threading.Thread(target=self._listen_udp, daemon=True)
        elif self.mode == "serial":
            t = threading.Thread(target=self._listen_serial, daemon=True)
        else:
            raise ValueError(f"מצב לא נתמך: {self.mode}")
        t.start()
        print(f"[RealGPS] מאזין ב-{self.mode.upper()} port={self.port}")

    def _listen_udp(self):
        import socket, json, hmac, hashlib
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((self.host, self.port))
        sock.settimeout(1.0)
        print(f"[RealGPS] UDP socket פתוח על {self.host}:{self.port}")
        while self._running:
            try:
                data, _ = sock.recvfrom(1024)
                msg = json.loads(data.decode())
                # כאן תבוא בדיקת חתימה דיגיטלית לפי ההצעה
                event = GPSEmergencyEvent(
                    active=msg.get("active", True),
                    lane_id=int(msg.get("lane_id", 0)),
                    vehicle_id=str(msg.get("vehicle_id", "UNKNOWN")),
                    timestamp=float(msg.get("timestamp", time.time())),
                )
                if self._callback:
                    self._callback(event)
            except Exception:
                pass

    def _listen_serial(self):
        """מוכן לחיבור עתידי — ממתין לפורט COM."""
        try:
            import serial, json
            ser = serial.Serial(self.port, 9600, timeout=1)
            print(f"[RealGPS] Serial port {self.port} פתוח")
            while self._running:
                line = ser.readline().decode().strip()
                if line:
                    msg = json.loads(line)
                    event = GPSEmergencyEvent(
                        active=msg.get("active", True),
                        lane_id=int(msg.get("lane_id", 0)),
                        vehicle_id=str(msg.get("vehicle_id", "UNKNOWN")),
                    )
                    if self._callback:
                        self._callback(event)
        except ImportError:
            print("[RealGPS] ❌ pyserial לא מותקן — הרץ: pip install pyserial")

    def stop(self):
        self._running = False


class RealLEDSensor(LEDSensorInterface):
    """
    חיישן LED אמיתי דרך GPIO (Raspberry Pi) או Arduino סריאלי.
    מוכן לחיבור — ימומש כשהחומרה תגיע.
    """

    def __init__(self, intersection_id: int, mode: str = "gpio"):
        self.intersection_id = intersection_id
        self.mode = mode
        print(f"[RealLED] חיישן LED אמיתי — {mode} (מוכן לחיבור)")

    def read(self, lane_id: int) -> LEDSensorReading:
        if self.mode == "gpio":
            try:
                import RPi.GPIO as GPIO
                pin = 17 + lane_id  # PIN לפי מפת החיבורים
                GPIO.setmode(GPIO.BCM)
                GPIO.setup(pin, GPIO.IN)
                present = GPIO.input(pin) == GPIO.HIGH
                return LEDSensorReading(lane_id=lane_id, vehicle_present=present)
            except ImportError:
                print("[RealLED] ❌ RPi.GPIO לא זמין — בדוק שהמחשב הוא Raspberry Pi")
                return LEDSensorReading(lane_id=lane_id, vehicle_present=False)
        return LEDSensorReading(lane_id=lane_id, vehicle_present=False)

    def is_connected(self) -> bool:
        try:
            import RPi.GPIO
            return True
        except ImportError:
            return False


# ══════════════════════════════════════════════════════════════
# Factory — בחר Mock או Real בשורה אחת
# ══════════════════════════════════════════════════════════════

# ◀── שנה כאן ל-True כשהחומרה האמיתית מחוברת ──▶
USE_REAL_HARDWARE = False


def get_camera(intersection_id: int, camera_source=None, num_lanes: int = 4) -> CameraInterface:
    """מחזיר מצלמה — Mock בפיתוח, Real בייצור."""
    if USE_REAL_HARDWARE and camera_source is not None:
        return RealCamera(intersection_id, camera_source, num_lanes)
    return MockCamera(intersection_id, num_lanes)


def get_gps_button(intersection_id: int, **kwargs) -> GPSButtonInterface:
    """מחזיר לחצן GPS — Mock בפיתוח, Real בייצור."""
    if USE_REAL_HARDWARE:
        return RealGPSButton(intersection_id, **kwargs)
    return MockGPSButton(intersection_id)


def get_led_sensor(intersection_id: int, **kwargs) -> LEDSensorInterface:
    """מחזיר חיישן LED — Mock בפיתוח, Real בייצור."""
    if USE_REAL_HARDWARE:
        return RealLEDSensor(intersection_id, **kwargs)
    return MockLEDSensor(intersection_id)
