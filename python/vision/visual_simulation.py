import time
import random
import cv2
import numpy as np

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


def run_visual_simulation(
    intersection_id: int = 1,
    num_lanes: int = 4,
    server_url: str = "http://127.0.0.1:8000/state",
):
    width, height = 1000, 600
    lane_zones = build_lane_zones(width, height, num_lanes)
    waiting_times = [0.0] * num_lanes

    print("Visual simulation started")
    print("Press Q to exit")

    while True:
        frame = np.zeros((height, width, 3), dtype=np.uint8)
        frame[:] = (30, 30, 30)

        lanes = []
        for lane_id, (x1, y1, x2, y2) in enumerate(lane_zones):
            x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)

            vehicle_count = random.randint(0, 12)
            pedestrian_count = random.randint(0, 4)
            density_pct = min(100.0, vehicle_count * random.uniform(5.0, 12.0))

            if vehicle_count > 0:
                waiting_times[lane_id] += 1.0
            else:
                waiting_times[lane_id] = 0.0

            lane = LaneState(
                lane_id=lane_id,
                vehicle_count=vehicle_count,
                pedestrian_count=pedestrian_count,
                density_pct=round(density_pct, 1),
                waiting_time_sec=waiting_times[lane_id],
            )
            lanes.append(lane)

            color = (0, 180, 255) if density_pct >= 70 else (0, 200, 0)
            cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)

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
                f"V={vehicle_count} P={pedestrian_count} D={lane.density_pct}% W={lane.waiting_time_sec:.0f}s",
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

        cv2.putText(
            frame,
            f"SIMULATION MODE | intersection={intersection_id} | lanes={num_lanes} | RL size={len(state.to_rl_vector())}",
            (15, 30),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.7,
            (255, 255, 255),
            2,
        )

        cv2.imshow("Smart Traffic Visual Simulation", frame)
        key = cv2.waitKey(1000) & 0xFF
        if key in (ord("q"), ord("Q")):
            break

    cv2.destroyAllWindows()


if __name__ == "__main__":
    run_visual_simulation(intersection_id=1, num_lanes=4)
