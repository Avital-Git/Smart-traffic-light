from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field
from typing import Dict, Optional, List


class EmergencySignal(BaseModel):
    active: bool = Field(..., description="האם יש כעת בקשת חירום")
    lane_direction: Optional[str] = Field(None, description="נתיב הרכב החירום: N/S/E/W")
    vehicle_id: Optional[str] = Field(None, description="מזהה רכב חירום")
    timestamp: Optional[float] = Field(None, description="זמן קבלת האות")


class LaneState(BaseModel):
    lane_id: int
    vehicle_count: int
    pedestrian_count: int
    density_pct: float
    waiting_time_sec: float


class IntersectionState(BaseModel):
    intersection_id: int
    num_lanes: int
    timestamp: float
    lanes: List[LaneState]
    emergency_signal: Optional[EmergencySignal] = None
    neighbor_states: Optional[Dict[str, dict]] = None


class ActionResponse(BaseModel):
    action: str
    reason: str
    intersection_id: int


app = FastAPI(
    title="Smart Traffic Server",
    description="Backend API עבור פרויקט ניהול תנועה חכמה",
    version="0.1.0",
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# אחסון מצב פשוט בזיכרון לצורך פיתוח ראשוני
state_store: Dict[int, IntersectionState] = {}


def decide_action(state: IntersectionState) -> ActionResponse:
    """החלטה על פעולה בהתאם למצב הצומת."""
    if state.emergency_signal and state.emergency_signal.active:
        lane_id = state.emergency_signal.lane_id
        if 0 <= lane_id < state.num_lanes:
            return ActionResponse(
                intersection_id=state.intersection_id,
                action=f"Green{lane_id}",
                reason="בקשת חירום פעילה",
            )

    # בחירה פשוטה של הנתיב עם הכי הרבה רכבים
    best_lane_id = 0
    best_score = -1.0
    for lane in state.lanes:
        score = lane.vehicle_count * 1.5 + lane.density_pct * 0.5 + lane.waiting_time_sec * 0.2
        if score > best_score:
            best_score = score
            best_lane_id = lane.lane_id

    return ActionResponse(
        intersection_id=state.intersection_id,
        action=f"Green{best_lane_id}",
        reason=f"נתיב {best_lane_id} עם העומס הגבוה ביותר",
    )


@app.get("/health")
def health_check():
    return {"status": "ok", "message": "Smart Traffic Server running"}


@app.post("/state", response_model=ActionResponse)
def submit_state(state: IntersectionState):
    state_store[state.intersection_id] = state
    action = decide_action(state)
    return action


@app.get("/intersection/{intersection_id}")
def get_intersection_state(intersection_id: int):
    state = state_store.get(intersection_id)
    if state is None:
        raise HTTPException(status_code=404, detail="Intersection state not found")
    return state


@app.get("/intersection/{intersection_id}/action", response_model=ActionResponse)
def get_last_action(intersection_id: int):
    state = state_store.get(intersection_id)
    if state is None:
        raise HTTPException(status_code=404, detail="Intersection state not found")
    return decide_action(state)
