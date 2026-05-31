import time
import random
import cv2
import numpy as np
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

try:
    from vision.intersection_vision import (
        LaneState,
        IntersectionState,
        build_lane_zones,
        post_state_to_server,
    )
except ModuleNotFoundError:
    from intersection_vision import (
        LaneState,
        IntersectionState,
        build_lane_zones,
        post_state_to_server,
    )

try:
    from hardware_interface import get_camera, get_gps_button, GPSEmergencyEvent
    _HW_AVAILABLE = True
except ImportError:
    _HW_AVAILABLE = False

# אות חירום פעיל — מתעדכן על ידי callback מ-GPS
_active_emergency: dict = {"event": None}


def _on_emergency(event: "GPSEmergencyEvent"):
    """נקרא אוטומטית כשרכב חירום לוחץ על הלחצן GPS."""
    _active_emergency["event"] = event
    print(f"[Simulation] 🚨 חירום! נתיב {event.direction} | רכב {event.vehicle_id}")


def run_visual_simulation(
    intersection_id: int = 1,
    num_lanes: int = 4,
    server_url: str = "http://127.0.0.1:8000/state",
):
    width, height = 1000, 600
    lane_zones = build_lane_zones(width, height, num_lanes)

    # אתחול חיישנים — Mock אם אין חומרה, Real אם יש
    if _HW_AVAILABLE:
        camera = get_camera(intersection_id=intersection_id, num_lanes=num_lanes)
        gps_btn = get_gps_button(intersection_id=intersection_id)
        gps_btn.listen(_on_emergency)
        print(f"[Simulation] מצלמה ו-GPS מוכנים (USE_REAL_HARDWARE={__import__('hardware_interface').USE_REAL_HARDWARE})")
        print("[Simulation] לחץ E להפעיל אות חירום מדומה | Q ליציאה")
    else:
        camera = None
        gps_btn = None
        print("[Simulation] ⚠️  hardware_interface לא נמצא — משתמש בנתונים אקראיים פשוטים")

    waiting_times = [0.0] * num_lanes

    print("Visual simulation started")
    print("Press Q to exit")

    while True:
        frame = np.zeros((height, width, 3), dtype=np.uint8)
        frame[:] = (30, 30, 30)

        lanes = []
        for lane_id, (x1, y1, x2, y2) in enumerate(lane_zones):
            x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)

            # שליפת נתונים מהחיישן (Mock או Real)
            if camera is not None:
                cam_frame = camera.read_frame(lane_id)
                vehicle_count = cam_frame.vehicle_count if cam_frame else random.randint(0, 8)
                density_pct = cam_frame.density_pct if cam_frame else min(100.0, vehicle_count * 9.0)
            else:
                vehicle_count = random.randint(0, 12)
                density_pct = min(100.0, vehicle_count * random.uniform(7.0, 12.0))

            if vehicle_count > 0:
                waiting_times[lane_id] += 1.0
            else:
                waiting_times[lane_id] = 0.0

            lane = LaneState(
                lane_id=lane_id,
                vehicle_count=vehicle_count,
                density_pct=round(density_pct, 1),
                waiting_time_sec=waiting_times[lane_id],
            )
            lanes.append(lane)

            # צבע לפי עומס + הדגשה אדומה אם יש חירום בנתיב זה
            emergency = _active_emergency.get("event")
            is_emergency_lane = (emergency and emergency.active and emergency.lane_id == lane_id)
            if is_emergency_lane:
                color = (0, 0, 255)
            elif lane.density_pct >= 70:
                color = (0, 180, 255)
            else:
                color = (0, 200, 0)

            cv2.rectangle(frame, (x1, y1), (x2, y2), color, 3 if is_emergency_lane else 2)

            cv2.putText(
                frame,
                f"Lane {lane_id}",
                (x1 + 8, y1 + 22),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.6,
                (255, 255, 255),
                2,
            )
            cv2.putText(
                frame,
                f"Count={vehicle_count} D={lane.density_pct:.1f}% W={lane.waiting_time_sec:.0f}s",
                (x1 + 8, y1 + 48),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.5,
                (220, 220, 220),
                1,
            )

            # draw simple simulated vehicles
            for _ in range(min(vehicle_count, 8)):
                px = random.randint(max(x1 + 10, 0), max(x1 + 10, min(x2 - 10, width - 1)))
                py = random.randint(max(y1 + 55, 0), max(y1 + 55, min(y2 - 10, height - 1)))
                cv2.circle(frame, (px, py), 5, (0, 255, 255), -1)

        state = IntersectionState(
            intersection_id=intersection_id,
            num_lanes=num_lanes,
            timestamp=time.time(),
            lanes=lanes,
        )

        # Post to server (if running)
        post_state_to_server(state, server_url)

        # שורת כותרת — מצב חירום פעיל?
        emergency = _active_emergency.get("event")
        if emergency and emergency.active:
            cv2.rectangle(frame, (0, 0), (width, 50), (0, 0, 180), -1)
            cv2.putText(
                frame,
                f"GPS EMERGENCY! נתיב {emergency.direction} | {emergency.vehicle_id}",
                (15, 35),
                cv2.FONT_HERSHEY_SIMPLEX, 0.9, (255, 255, 255), 2,
            )
        else:
            hw_label = "REAL HW" if (_HW_AVAILABLE and __import__('hardware_interface').USE_REAL_HARDWARE) else "MOCK"
            cv2.putText(
                frame,
                f"SIMULATION [{hw_label}] | intersection={intersection_id} | lanes={num_lanes}",
                (15, 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2,
            )

        cv2.imshow("Smart Traffic Visual Simulation", frame)
        key = cv2.waitKey(1000) & 0xFF
        if key in (ord("q"), ord("Q")):
            break
        elif key in (ord("e"), ord("E")) and gps_btn is not None:
            # לחיצת E מדמה אות חירום
            gps_btn.simulate_press(lane_id=random.randint(0, num_lanes - 1))
        elif key in (ord("c"), ord("C")):
            # לחיצת C מנקה אות חירום
            _active_emergency["event"] = None
            print("[Simulation] אות חירום נוקה")

    if gps_btn:
        gps_btn.stop()
    if camera:
        camera.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    run_visual_simulation(intersection_id=1, num_lanes=4)
