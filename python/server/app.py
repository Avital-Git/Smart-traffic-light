from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field
from typing import Dict, Optional, List, Set, Any
import re
import time
import json
import hmac
import hashlib
import logging
from pathlib import Path
import os

# Import security modules (support both package and direct execution)
try:
    from .security_config import get_security_config
    from .security_middleware import (
        SecurityHeadersMiddleware,
        RateLimitMiddleware,
        APIKeyValidationMiddleware,
        RequestLoggingMiddleware,
    )
except ImportError:
    try:
        from security_config import get_security_config
        from security_middleware import (
            SecurityHeadersMiddleware,
            RateLimitMiddleware,
            APIKeyValidationMiddleware,
            RequestLoggingMiddleware,
        )
    except ImportError:
        # Fallback if security modules not available
        get_security_config = None
        SecurityHeadersMiddleware = None
        RateLimitMiddleware = None
        APIKeyValidationMiddleware = None
        RequestLoggingMiddleware = None
# Setup logging first
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger(__name__)

# Import config loader (support both package and direct execution)
try:
    from .config_loader import get_config_loader
except Exception:
    try:
        from config_loader import get_config_loader
    except Exception as e:
        logger.warning(f"Failed to import config_loader: {e}")
        get_config_loader = None

try:
    from db_intersections import fetch_intersections as fetch_db_intersections
except Exception:
    fetch_db_intersections = None


class EmergencySignal(BaseModel):
    active: bool = Field(..., description="האם יש כעת בקשת חירום")
    lane_id: Optional[int] = Field(None, description="מזהה נתיב רכב החירום")
    vehicle_id: Optional[str] = Field(None, description="מזהה רכב חירום")
    timestamp: Optional[float] = Field(None, description="זמן קבלת האות")
    signature: Optional[str] = Field(None, description="חתימת HMAC-SHA256 לאימות אות חירום")


class LaneState(BaseModel):
    lane_id: int
    vehicle_count: int
    density_pct: float = 0.0
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
    phase_id: Optional[int] = None


class ControllerActionUpdate(BaseModel):
    action: str = Field(..., description="פעולה שנבחרה בבקר C++")
    phase_id: Optional[int] = Field(default=None, description="מזהה פאזה (אופציונלי, עדיף)")
    reason: Optional[str] = Field(default="selected_by_cpp_controller", description="נימוק אופציונלי")
    timestamp: Optional[float] = Field(default=None, description="זמן החלטת הבקר")


class IntersectionSummary(BaseModel):
    id: int
    code: str
    name: str
    city: str


class NeighborPacketSummary(BaseModel):
    intersection_id: int
    action: str
    phase_id: Optional[int] = None
    total_queue: int
    avg_waiting_sec: float
    emergency_active: bool = False
    signed_at: float = 0.0
    signature: Optional[str] = None


app = FastAPI(
    title="Smart Traffic Server",
    description="Backend API עבור פרויקט ניהול תנועה חכמה",
    version="0.1.0",
    docs_url="/docs" if os.getenv("TRAFFIC_ENV", "dev") == "dev" else None,
    redoc_url="/redoc" if os.getenv("TRAFFIC_ENV", "dev") == "dev" else None,
)

# Load security configuration
try:
    if callable(get_security_config):
        security_config = get_security_config()
        logger.info(f"Security config: {security_config.get_config_dict()}")
    else:
        security_config = None
except Exception as e:
    logger.error(f"Failed to load security config: {e}")
    security_config = None

# Load traffic configuration
try:
    if callable(get_config_loader):
        config_loader = get_config_loader()
        if config_loader:
            logger.info(f"Traffic configuration: {config_loader.get_config_summary()}")
    else:
        config_loader = None
except Exception as e:
    logger.error(f"Failed to load traffic config: {e}")
    config_loader = None

# Configure CORS based on security config
cors_origins = ["*"]
if security_config:
    cors_origins = security_config.cors_origins

app.add_middleware(
    CORSMiddleware,
    allow_origins=cors_origins,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Add security middleware stack (in reverse order of application)
if security_config:
    # Request logging
    if security_config.log_level == "DEBUG" and RequestLoggingMiddleware is not None:
        app.add_middleware(RequestLoggingMiddleware)
    
    # Security headers
    if security_config.security_headers_enabled and SecurityHeadersMiddleware is not None:
        app.add_middleware(SecurityHeadersMiddleware)
    
    # Rate limiting
    if security_config.rate_limit_enabled and RateLimitMiddleware is not None:
        app.add_middleware(
            RateLimitMiddleware,
            requests_per_minute=security_config.rate_limit_requests_per_minute
        )
    
    # API key validation (for production)
    if security_config.api_key_enabled and security_config.api_keys and APIKeyValidationMiddleware is not None:
        app.add_middleware(
            APIKeyValidationMiddleware,
            api_keys=security_config.api_keys,
            enabled=True
        )

# אחסון מצב פשוט בזיכרון לצורך פיתוח ראשוני
state_store: Dict[int, IntersectionState] = {}
action_store: Dict[int, ActionResponse] = {}

DEFAULT_INTERSECTIONS: List[IntersectionSummary] = [
    IntersectionSummary(id=1, code="J-001", name="צומת מרכזי", city="באר שבע"),
    IntersectionSummary(id=2, code="J-002", name="צומת האוניברסיטה", city="באר שבע"),
    IntersectionSummary(id=3, code="J-003", name="צומת בית חולים", city="באר שבע"),
    IntersectionSummary(id=4, code="J-004", name="צומת דרומי", city="באר שבע"),
]

DEFAULT_NEIGHBOR_TOPOLOGY: Dict[int, List[int]] = {
    1: [2],
    2: [1, 3],
    3: [2, 4],
    4: [3],
}

DEFAULT_EMERGENCY_KEYS: Dict[str, str] = {
    "AMB001": "demo-emergency-key-001",
    "POL001": "demo-emergency-key-002",
}

DEFAULT_MAX_EMERGENCY_CLOCK_SKEW_SEC = 30.0
DEFAULT_NEIGHBOR_MESSAGE_KEY = "demo-neighbor-message-key"
DEFAULT_NEIGHBOR_SIGNATURE_SKEW_SEC = 10.0


_PHASE_ACTION_RE = re.compile(r"^Phase(\d+)$")

# Replay protection: keep most recent accepted timestamp per emergency vehicle.
_emergency_last_timestamp_by_vehicle: Dict[str, float] = {}


class LiveUpdateHub:
    """In-memory WebSocket hub for live UI/controller updates."""

    def __init__(self) -> None:
        self._all_clients: Set[WebSocket] = set()
        self._by_intersection: Dict[int, Set[WebSocket]] = {}

    async def connect(self, websocket: WebSocket, intersection_id: Optional[int] = None) -> None:
        await websocket.accept()
        self._all_clients.add(websocket)
        if intersection_id is not None:
            self._by_intersection.setdefault(intersection_id, set()).add(websocket)

    def disconnect(self, websocket: WebSocket, intersection_id: Optional[int] = None) -> None:
        self._all_clients.discard(websocket)
        if intersection_id is not None:
            clients = self._by_intersection.get(intersection_id)
            if clients is not None:
                clients.discard(websocket)
                if not clients:
                    self._by_intersection.pop(intersection_id, None)
            return

        stale_keys: List[int] = []
        for iid, clients in self._by_intersection.items():
            clients.discard(websocket)
            if not clients:
                stale_keys.append(iid)

        for iid in stale_keys:
            self._by_intersection.pop(iid, None)

    async def broadcast(self, event: str, payload: Dict[str, Any], intersection_id: Optional[int] = None) -> None:
        if intersection_id is None:
            targets = list(self._all_clients)
        else:
            scoped = self._by_intersection.get(intersection_id, set())
            targets = list(set(scoped) | self._all_clients)

        message = {
            "event": event,
            "intersection_id": intersection_id,
            "timestamp": time.time(),
            "payload": payload,
        }

        for ws in targets:
            try:
                await ws.send_json(message)
            except Exception:
                self.disconnect(ws)


LIVE_UPDATES = LiveUpdateHub()


def load_neighbor_topology() -> Dict[int, List[int]]:
    candidate_paths = [
        Path("python/server/neighbor_topology.json"),
        Path("neighbor_topology.json"),
    ]

    for path in candidate_paths:
        try:
            if not path.exists():
                continue

            payload = json.loads(path.read_text(encoding="utf-8"))
            intersections = payload.get("intersections", {})
            topology: Dict[int, List[int]] = {}
            for intersection_id_str, entry in intersections.items():
                try:
                    intersection_id = int(intersection_id_str)
                except (TypeError, ValueError):
                    continue

                raw_neighbors = entry.get("neighbors", []) if isinstance(entry, dict) else []
                neighbors: List[int] = []
                for neighbor_id in raw_neighbors:
                    try:
                        neighbors.append(int(neighbor_id))
                    except (TypeError, ValueError):
                        continue

                topology[intersection_id] = neighbors

            if topology:
                return topology
        except Exception:
            continue

    return DEFAULT_NEIGHBOR_TOPOLOGY


NEIGHBOR_TOPOLOGY = load_neighbor_topology()


def load_emergency_auth_config() -> tuple[Dict[str, str], float, str]:
    candidate_paths = [
        Path("python/server/emergency_keys.json"),
        Path("emergency_keys.json"),
    ]

    for path in candidate_paths:
        try:
            if not path.exists():
                continue

            payload = json.loads(path.read_text(encoding="utf-8"))
            vehicle_keys_raw = payload.get("vehicle_keys", {})
            if not isinstance(vehicle_keys_raw, dict):
                continue

            vehicle_keys: Dict[str, str] = {}
            for vehicle_id, secret in vehicle_keys_raw.items():
                if not isinstance(vehicle_id, str) or not isinstance(secret, str):
                    continue
                if not vehicle_id.strip() or not secret:
                    continue
                vehicle_keys[vehicle_id.strip()] = secret

            max_skew = payload.get("max_clock_skew_sec", DEFAULT_MAX_EMERGENCY_CLOCK_SKEW_SEC)
            try:
                max_skew = float(max_skew)
            except (TypeError, ValueError):
                max_skew = DEFAULT_MAX_EMERGENCY_CLOCK_SKEW_SEC

            max_skew = max(1.0, max_skew)

            if vehicle_keys:
                return vehicle_keys, max_skew, str(path)
        except Exception:
            continue

    return DEFAULT_EMERGENCY_KEYS, DEFAULT_MAX_EMERGENCY_CLOCK_SKEW_SEC, "defaults"


EMERGENCY_KEYS, EMERGENCY_MAX_CLOCK_SKEW_SEC, EMERGENCY_KEYS_SOURCE = load_emergency_auth_config()


def load_neighbor_message_auth_config() -> tuple[str, float, str]:
    candidate_paths = [
        Path("python/server/neighbor_message_auth.json"),
        Path("neighbor_message_auth.json"),
    ]

    for path in candidate_paths:
        try:
            if not path.exists():
                continue

            payload = json.loads(path.read_text(encoding="utf-8"))
            key = payload.get("shared_hmac_key")
            if not isinstance(key, str) or not key:
                continue

            max_skew = payload.get("max_signature_skew_sec", DEFAULT_NEIGHBOR_SIGNATURE_SKEW_SEC)
            try:
                max_skew = float(max_skew)
            except (TypeError, ValueError):
                max_skew = DEFAULT_NEIGHBOR_SIGNATURE_SKEW_SEC

            max_skew = max(1.0, max_skew)
            return key, max_skew, str(path)
        except Exception:
            continue

    return DEFAULT_NEIGHBOR_MESSAGE_KEY, DEFAULT_NEIGHBOR_SIGNATURE_SKEW_SEC, "defaults"


NEIGHBOR_MESSAGE_KEY, NEIGHBOR_SIGNATURE_SKEW_SEC, NEIGHBOR_MESSAGE_AUTH_SOURCE = load_neighbor_message_auth_config()


def sign_neighbor_summary(summary: NeighborPacketSummary) -> NeighborPacketSummary:
    summary.signed_at = time.time()
    payload = (
        f"{summary.intersection_id}|{summary.phase_id if summary.phase_id is not None else -1}|"
        f"{summary.total_queue}|{summary.avg_waiting_sec:.3f}|"
        f"{1 if summary.emergency_active else 0}|{summary.signed_at:.3f}"
    )
    summary.signature = hmac.new(
        NEIGHBOR_MESSAGE_KEY.encode("utf-8"),
        payload.encode("utf-8"),
        hashlib.sha256,
    ).hexdigest()
    return summary


def _canonical_emergency_payload(signal: EmergencySignal) -> str:
    lane_id = signal.lane_id if signal.lane_id is not None else -1
    ts = signal.timestamp if signal.timestamp is not None else 0.0
    return f"{signal.vehicle_id}|{lane_id}|{ts:.3f}"


def validate_emergency_signal(signal: Optional[EmergencySignal]) -> tuple[bool, str]:
    if signal is None or not signal.active:
        return True, "inactive_or_missing"

    if signal.lane_id is None or signal.lane_id < 0:
        return False, "invalid_lane_id"

    if not signal.vehicle_id:
        return False, "missing_vehicle_id"

    if signal.timestamp is None:
        return False, "missing_timestamp"

    if not signal.signature:
        return False, "missing_signature"

    now = time.time()
    if abs(now - signal.timestamp) > EMERGENCY_MAX_CLOCK_SKEW_SEC:
        return False, "timestamp_out_of_window"

    last_ts = _emergency_last_timestamp_by_vehicle.get(signal.vehicle_id)
    if last_ts is not None and signal.timestamp <= last_ts:
        return False, "replay_detected"

    secret = EMERGENCY_KEYS.get(signal.vehicle_id) or EMERGENCY_KEYS.get("*")
    if not secret:
        return False, "unknown_vehicle_id"

    payload = _canonical_emergency_payload(signal)
    expected_sig = hmac.new(secret.encode("utf-8"), payload.encode("utf-8"), hashlib.sha256).hexdigest()

    if not hmac.compare_digest(signal.signature.strip().lower(), expected_sig.lower()):
        return False, "invalid_signature"

    _emergency_last_timestamp_by_vehicle[signal.vehicle_id] = signal.timestamp
    return True, "ok"


def normalize_controller_action(action_text: str, phase_id: Optional[int]) -> tuple[str, Optional[int]]:
    """
    Normalize and validate controller action format.

    Allowed formats:
    - action="PhaseX" (with optional matching phase_id)
    - action="Hold" (phase_id must be None or -1)
    """
    action_text = action_text.strip()

    if action_text == "Hold":
        if phase_id not in (None, -1):
            raise HTTPException(status_code=400, detail="For Hold action, phase_id must be null or -1")
        return "Hold", None

    m = _PHASE_ACTION_RE.match(action_text)
    if m:
        parsed_phase = int(m.group(1))
        if phase_id is not None and phase_id != parsed_phase:
            raise HTTPException(status_code=400, detail="action and phase_id mismatch")
        return f"Phase{parsed_phase}", parsed_phase

    if phase_id is not None and phase_id >= 0:
        return f"Phase{phase_id}", phase_id

    raise HTTPException(status_code=400, detail="Invalid action format. Use PhaseX or Hold")


def decide_action(state: IntersectionState) -> ActionResponse:
    """החלטה בסיסית בשרת (ה-RL הראשי רץ ב-C++)."""
    def lane_to_phase_id(lane_id: int) -> int:
        # מיפוי ברירת מחדל תואם לבניית הפאזות בצד C++ (זוגי/אי-זוגי)
        return 0 if (lane_id % 2 == 0) else 1

    if state.emergency_signal and state.emergency_signal.active:
        lane_id = state.emergency_signal.lane_id
        if lane_id is not None and 0 <= lane_id < state.num_lanes:
            phase_id = lane_to_phase_id(lane_id)
            return ActionResponse(
                intersection_id=state.intersection_id,
                action=f"Phase{phase_id}",
                reason="בקשת חירום פעילה",
                phase_id=phase_id,
            )

    best_lane_id = 0
    best_score = -1.0
    for lane in state.lanes:
        score = lane.vehicle_count * 1.5 + lane.density_pct * 0.5 + lane.waiting_time_sec * 0.2
        if score > best_score:
            best_score = score
            best_lane_id = lane.lane_id

    best_phase_id = lane_to_phase_id(best_lane_id)
    return ActionResponse(
        intersection_id=state.intersection_id,
        action=f"Phase{best_phase_id}",
        reason=f"פאזה {best_phase_id} נבחרה לפי העומס הגבוה ביותר",
        phase_id=best_phase_id,
    )


def summarize_neighbor(intersection_id: int) -> Optional[NeighborPacketSummary]:
    neighbor_state = state_store.get(intersection_id)
    if neighbor_state is None:
        return None

    neighbor_action = action_store.get(intersection_id)
    if neighbor_action is None:
        neighbor_action = decide_action(neighbor_state)

    total_queue = sum(max(0, lane.vehicle_count) for lane in neighbor_state.lanes)
    avg_waiting_sec = (
        sum(max(0.0, lane.waiting_time_sec) for lane in neighbor_state.lanes) / len(neighbor_state.lanes)
        if neighbor_state.lanes else 0.0
    )

    summary = NeighborPacketSummary(
        intersection_id=intersection_id,
        action=neighbor_action.action,
        phase_id=neighbor_action.phase_id,
        total_queue=total_queue,
        avg_waiting_sec=avg_waiting_sec,
        emergency_active=bool(neighbor_state.emergency_signal and neighbor_state.emergency_signal.active),
    )
    return sign_neighbor_summary(summary)


def build_neighbor_summaries(intersection_id: int) -> List[NeighborPacketSummary]:
    summaries: List[NeighborPacketSummary] = []
    for neighbor_id in NEIGHBOR_TOPOLOGY.get(intersection_id, []):
        summary = summarize_neighbor(neighbor_id)
        if summary is not None:
            summaries.append(summary)
    return summaries


def load_intersections() -> List[IntersectionSummary]:
    if fetch_db_intersections is None:
        return DEFAULT_INTERSECTIONS

    try:
        rows = fetch_db_intersections()
        if not rows:
            return DEFAULT_INTERSECTIONS

        intersections: List[IntersectionSummary] = []
        for row in rows:
            intersections.append(
                IntersectionSummary(
                    id=int(row.get("intersection_id")),
                    code=str(row.get("intersection_code") or f"J-{int(row.get('intersection_id')):03d}"),
                    name=str(row.get("name") or f"צומת {row.get('intersection_id')}"),
                    city=str(row.get("city") or "לא צוין"),
                )
            )
        return intersections
    except Exception:
        return DEFAULT_INTERSECTIONS


def build_metrics_summary() -> Dict[str, Any]:
    intersections: List[Dict[str, Any]] = []
    now = time.time()

    for intersection_id, state in state_store.items():
        total_queue = sum(max(0, lane.vehicle_count) for lane in state.lanes)
        avg_waiting_sec = (
            sum(max(0.0, lane.waiting_time_sec) for lane in state.lanes) / len(state.lanes)
            if state.lanes else 0.0
        )
        action = action_store.get(intersection_id)
        intersections.append(
            {
                "intersection_id": intersection_id,
                "num_lanes": state.num_lanes,
                "total_queue": total_queue,
                "avg_waiting_sec": avg_waiting_sec,
                "emergency_active": bool(state.emergency_signal and state.emergency_signal.active),
                "last_action": action.model_dump() if action is not None else None,
                "state_age_sec": max(0.0, now - state.timestamp),
            }
        )

    intersections.sort(key=lambda item: item["intersection_id"])

    return {
        "timestamp": now,
        "intersection_count": len(intersections),
        "total_network_queue": sum(item["total_queue"] for item in intersections),
        "avg_network_waiting_sec": (
            sum(item["avg_waiting_sec"] for item in intersections) / len(intersections)
            if intersections else 0.0
        ),
        "intersections": intersections,
        "security": {
            "emergency_keys_source": EMERGENCY_KEYS_SOURCE,
            "neighbor_message_auth_source": NEIGHBOR_MESSAGE_AUTH_SOURCE,
        },
    }


@app.get("/")
def root():
    security_info = {}
    if security_config:
        security_info = {
            "environment": security_config.env.value,
            "ssl_enabled": security_config.use_ssl,
            "rate_limiting": security_config.rate_limit_enabled,
            "security_headers": security_config.security_headers_enabled,
        }
    
    config_info = {}
    if config_loader:
        config_info = config_loader.get_config_summary()
    
    return {
        "status": "ok",
        "message": "Smart Traffic Server running",
        "docs": "/docs",
        "health": "/health",
        "intersections": "/intersections",
        "security": security_info,
        "configuration": config_info,
        "emergency_auth": {
            "keys_source": EMERGENCY_KEYS_SOURCE,
            "max_clock_skew_sec": EMERGENCY_MAX_CLOCK_SKEW_SEC,
        },
        "neighbor_auth": {
            "source": NEIGHBOR_MESSAGE_AUTH_SOURCE,
            "max_signature_skew_sec": NEIGHBOR_SIGNATURE_SKEW_SEC,
        },
    }


@app.get("/intersections", response_model=List[IntersectionSummary])
def get_intersections():
    return load_intersections()


@app.get("/config")
def get_configuration():
    """Get current traffic configuration."""
    
    # If config_loader failed to import, provide defaults
    if config_loader is None:
        return {
            "status": "ok",
            "message": "Using default configuration (file-based config loader not available)",
            "configuration": {
                "traffic_config_loaded": False,
                "phases_config_loaded": False,
                "conflicts_config_loaded": False,
            },
            "thresholds": {
                "queue_critical": 20,
                "queue_high": 15,
                "density_critical_pct": 85.0,
                "wait_time_critical_sec": 60,
            },
            "network": {
                "default_cycle_time_sec": 60,
                "min_phase_duration_sec": 10,
                "max_phase_duration_sec": 90,
            },
            "emergency": {
                "emergency_cycle_time_sec": 30,
                "priority_boost_factor": 2.0,
            },
            "rl_agent": {
                "learning_rate": 0.1,
                "discount_factor": 0.95,
                "epsilon_initial": 1.0,
                "epsilon_final": 0.05,
            },
        }
    
    return {
        "status": "ok",
        "configuration": config_loader.get_config_summary(),
        "thresholds": {
            "queue_critical": config_loader.get_thresholds().queue_critical,
            "queue_high": config_loader.get_thresholds().queue_high,
            "density_critical_pct": config_loader.get_thresholds().density_critical_pct,
            "wait_time_critical_sec": config_loader.get_thresholds().wait_time_critical_sec,
        },
        "network": {
            "default_cycle_time_sec": config_loader.get_network_config().default_cycle_time_sec,
            "min_phase_duration_sec": config_loader.get_network_config().min_phase_duration_sec,
            "max_phase_duration_sec": config_loader.get_network_config().max_phase_duration_sec,
        },
        "emergency": {
            "emergency_cycle_time_sec": config_loader.get_emergency_config().emergency_cycle_time_sec,
            "priority_boost_factor": config_loader.get_emergency_config().priority_boost_factor,
        },
        "rl_agent": {
            "learning_rate": config_loader.get_rl_agent_config().learning_rate,
            "discount_factor": config_loader.get_rl_agent_config().discount_factor,
            "epsilon_initial": config_loader.get_rl_agent_config().epsilon_initial,
            "epsilon_final": config_loader.get_rl_agent_config().epsilon_final,
        },
    }


@app.get("/health")
def health_check():
    return {"status": "ok", "message": "Smart Traffic Server running"}

@app.get("/metrics/summary")
def metrics_summary():
    return build_metrics_summary()


@app.post("/state", response_model=ActionResponse)
async def submit_state(state: IntersectionState):
    signal_ok, reason = validate_emergency_signal(state.emergency_signal)
    if not signal_ok:
        raise HTTPException(status_code=401, detail=f"Emergency signal rejected: {reason}")

    state_store[state.intersection_id] = state
    action = decide_action(state)
    # ברירת מחדל: החלטת שרת, עד ש-C++ ידווח פעולה עדכנית.
    action_store[state.intersection_id] = action

    await LIVE_UPDATES.broadcast(
        event="state_updated",
        intersection_id=state.intersection_id,
        payload={
            "state": state.model_dump(),
            "action": action.model_dump(),
        },
    )

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
    saved = action_store.get(intersection_id)
    if saved is not None:
        return saved
    return decide_action(state)


@app.post("/intersection/{intersection_id}/action", response_model=ActionResponse)
async def submit_controller_action(intersection_id: int, update: ControllerActionUpdate):
    state = state_store.get(intersection_id)
    if state is None:
        raise HTTPException(status_code=404, detail="Intersection state not found")

    action_text, phase_id = normalize_controller_action(update.action, update.phase_id)

    action = ActionResponse(
        intersection_id=intersection_id,
        action=action_text,
        reason=update.reason or "selected_by_cpp_controller",
        phase_id=phase_id,
    )
    action_store[intersection_id] = action

    await LIVE_UPDATES.broadcast(
        event="action_updated",
        intersection_id=intersection_id,
        payload={
            "action": action.model_dump(),
            "source": "cpp_controller",
        },
    )

    return action


@app.websocket("/ws/updates")
async def ws_updates(websocket: WebSocket):
    await LIVE_UPDATES.connect(websocket)
    try:
        await websocket.send_json(
            {
                "event": "welcome",
                "intersection_id": None,
                "timestamp": time.time(),
                "payload": {
                    "message": "connected_to_global_updates",
                },
            }
        )
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        LIVE_UPDATES.disconnect(websocket)
    except Exception:
        LIVE_UPDATES.disconnect(websocket)


@app.websocket("/ws/intersection/{intersection_id}")
async def ws_intersection_updates(websocket: WebSocket, intersection_id: int):
    await LIVE_UPDATES.connect(websocket, intersection_id)
    try:
        current_state = state_store.get(intersection_id)
        current_action = action_store.get(intersection_id)
        await websocket.send_json(
            {
                "event": "welcome",
                "intersection_id": intersection_id,
                "timestamp": time.time(),
                "payload": {
                    "message": "connected_to_intersection_updates",
                    "has_state": current_state is not None,
                    "state": current_state.model_dump() if current_state is not None else None,
                    "action": current_action.model_dump() if current_action is not None else None,
                },
            }
        )

        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        LIVE_UPDATES.disconnect(websocket, intersection_id)
    except Exception:
        LIVE_UPDATES.disconnect(websocket, intersection_id)


@app.get("/intersection/{intersection_id}/packet")
def get_intersection_packet(intersection_id: int):
    """חבילת עבודה מלאה לבקר C++: מצב נוכחי + פעולה אחרונה."""
    state = state_store.get(intersection_id)
    if state is None:
        raise HTTPException(status_code=404, detail="Intersection state not found")

    action = action_store.get(intersection_id)
    if action is None:
        action = decide_action(state)

    return {
        "timestamp": time.time(),
        "state": state,
        "last_action": action,
        "neighbors": [summary.model_dump() for summary in build_neighbor_summaries(intersection_id)],
    }
