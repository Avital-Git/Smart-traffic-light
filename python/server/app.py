from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect, Depends, status
from fastapi.middleware.cors import CORSMiddleware
from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials
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
import jwt
from datetime import datetime, timedelta

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
    from db_intersections import (
        fetch_intersections as fetch_db_intersections,
        insert_intersection as db_insert_intersection,
        update_intersection,
        fetch_intersection_by_id,
        fetch_neighbors,
    )
except Exception:
    fetch_db_intersections = None
    db_insert_intersection = None
    update_intersection = None
    fetch_intersection_by_id = None
    fetch_neighbors = None

try:
    from .validators import (
        validate_intersection_data,
        validate_intersection_name,
        validate_num_cameras,
        validate_city,
        validate_region,
        validate_description,
        validate_phase_id,
        ValidationError,
    )
except Exception:
    try:
        from validators import (
            validate_intersection_data,
            validate_intersection_name,
            validate_num_cameras,
            validate_city,
            validate_region,
            validate_description,
            validate_phase_id,
            ValidationError,
        )
    except Exception as e:
        logger.warning(f"Failed to import validators: {e}")
        validate_intersection_data = None
        validate_intersection_name = None
        validate_num_cameras = None
        validate_city = None
        validate_region = None
        validate_description = None
        validate_phase_id = None
        ValidationError = Exception


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


class SimulatedEmergencyRequest(BaseModel):
    lane_id: int = Field(default=0, ge=0, description="מזהה נתיב לבקשת חירום")
    vehicle_id: str = Field(default="AMB001", min_length=1, description="מזהה רכב חירום")


class IntersectionSummary(BaseModel):
    id: int
    code: str
    name: str
    city: str


class AdminLoginRequest(BaseModel):
    username: str = Field(..., min_length=1, max_length=50)
    password: str = Field(..., min_length=1)


class AdminTokenResponse(BaseModel):
    access_token: str
    token_type: str = "bearer"
    expires_in: int


class IntersectionCreateRequest(BaseModel):
    code: str = Field(..., description="קוד צומת")
    name: str = Field(..., description="שם צומת")
    latitude: float = Field(..., description="קו רוחב")
    longitude: float = Field(..., description="קו אורך")
    num_cameras: int = Field(default=4, ge=1, le=16)
    city: Optional[str] = Field(default=None, description="עיר")
    region: Optional[str] = Field(default=None, description="אזור")
    description: Optional[str] = Field(default=None, description="תיאור")


class IntersectionUpdateRequest(BaseModel):
    name: Optional[str] = None
    num_cameras: Optional[int] = Field(default=None, ge=1, le=16)
    city: Optional[str] = None
    region: Optional[str] = None
    description: Optional[str] = None


class ManualControlRequest(BaseModel):
    intersection_id: int = Field(..., description="מזהה צומת")
    phase_id: int = Field(..., ge=0, le=7, description="מזהה פאזה לטיפול ידני")
    reason: str = Field(default="manual_admin_control", description="סיבת השינוי")


class LaneCreateRequest(BaseModel):
    camera_index: int = Field(..., ge=0, description="Index of lane/camera in intersection")
    direction: str = Field(..., min_length=1, max_length=2, description="N/S/E/W/NE/NW/SE/SW")
    description: Optional[str] = Field(default=None, max_length=100)


class LaneUpdateRequest(BaseModel):
    direction: Optional[str] = Field(default=None, min_length=1, max_length=2)
    description: Optional[str] = Field(default=None, max_length=100)


class LaneConflictCreateRequest(BaseModel):
    lane_id_1: int = Field(..., gt=0, description="First lane ID in conflict pair")
    lane_id_2: int = Field(..., gt=0, description="Second lane ID in conflict pair")
    conflict_type: Optional[str] = Field(default="crossing", description="Type: crossing, merging, diverging, etc.")


class LaneConflictResponse(BaseModel):
    conflict_id: int
    intersection_id: int
    lane_id_1: int
    lane_id_2: int
    conflict_type: str
    created_at: Optional[str] = None


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

# ══════════════════════════════════════════════════════
# Admin Authentication Configuration
# ══════════════════════════════════════════════════════

ADMIN_JWT_SECRET = os.getenv("ADMIN_JWT_SECRET", "super-secret-admin-key-change-in-production")
ADMIN_JWT_ALGORITHM = "HS256"
ADMIN_JWT_EXPIRATION_MINUTES = 480  # 8 hours

# Simple admin credentials (in production, use a database)
# Default: username="admin", password="TrafficAdmin123"
ADMIN_CREDENTIALS = {
    "admin": hashlib.sha256("TrafficAdmin123".encode()).hexdigest()
}

security = HTTPBearer(auto_error=False)


def verify_admin_token(credentials: Optional[HTTPAuthorizationCredentials] = Depends(security)) -> Dict[str, Any]:
    """Verify JWT token for admin endpoints."""
    if not credentials:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Missing authentication credentials",
        )
    
    try:
        payload = jwt.decode(
            credentials.credentials,
            ADMIN_JWT_SECRET,
            algorithms=[ADMIN_JWT_ALGORITHM]
        )
        username: str = payload.get("sub")
        if username is None:
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="Invalid token",
            )
    except jwt.ExpiredSignatureError:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Token expired",
        )
    except jwt.InvalidTokenError:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid token",
        )
    
    return {"username": username}


def create_admin_token(username: str) -> str:
    """Create JWT token for admin."""
    expire = datetime.utcnow() + timedelta(minutes=ADMIN_JWT_EXPIRATION_MINUTES)
    payload = {
        "sub": username,
        "exp": expire,
        "iat": datetime.utcnow()
    }
    token = jwt.encode(payload, ADMIN_JWT_SECRET, algorithm=ADMIN_JWT_ALGORITHM)
    return token


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
    logger.info(f"CORS will be configured with origins: {cors_origins}")
else:
    logger.warning("No security_config found, using default CORS origins")

# Always add common localhost variants to CORS
if cors_origins != ["*"]:
    common_hosts = [
        "http://localhost:3000",
        "http://localhost:8000", 
        "http://127.0.0.1:3000",
        "http://127.0.0.1:8000",
        "http://localhost:3001",
        "http://127.0.0.1:3001"
    ]
    for host in common_hosts:
        if host not in cors_origins:
            cors_origins.append(host)

logger.info(f"Final CORS origins: {cors_origins}")

app.add_middleware(
    CORSMiddleware,
    allow_origins=cors_origins,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
    max_age=3600,
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
action_source_store: Dict[int, str] = {}
action_updated_at_store: Dict[int, float] = {}

# Keep recent C++ actions stable; avoid replacing them every second with server fallback.
CPP_ACTION_STICKY_SEC = 5.0
MANUAL_ACTION_STICKY_SEC = 20.0

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


def is_real_hardware_mode() -> bool:
    """Detect whether runtime is connected to real hardware sensors."""
    value = str(os.getenv("TRAFFIC_USE_REAL_HARDWARE", "false")).strip().lower()
    return value in {"1", "true", "yes", "on"}


_PHASE_ACTION_RE = re.compile(r"^Phase(\d+)$")

# Replay protection: keep most recent accepted timestamp per emergency vehicle.
_emergency_last_timestamp_by_vehicle: Dict[str, float] = {}

# Emergency latching: keeps emergency active for a short TTL so frequent
# simulation state updates without emergency payload do not immediately clear it.
EMERGENCY_LATCH_SEC = 8.0
_emergency_latch_by_intersection: Dict[int, EmergencySignal] = {}
_emergency_latch_expiry_by_intersection: Dict[int, float] = {}


def _clear_emergency_latch(intersection_id: int) -> None:
    _emergency_latch_by_intersection.pop(intersection_id, None)
    _emergency_latch_expiry_by_intersection.pop(intersection_id, None)


def _set_emergency_latch(intersection_id: int, signal: EmergencySignal) -> None:
    _emergency_latch_by_intersection[intersection_id] = signal.model_copy(deep=True)
    _emergency_latch_expiry_by_intersection[intersection_id] = time.time() + EMERGENCY_LATCH_SEC


def _get_latched_emergency(intersection_id: int) -> Optional[EmergencySignal]:
    expires_at = _emergency_latch_expiry_by_intersection.get(intersection_id)
    if expires_at is None:
        return None
    if time.time() > expires_at:
        _clear_emergency_latch(intersection_id)
        return None
    signal = _emergency_latch_by_intersection.get(intersection_id)
    return signal.model_copy(deep=True) if signal is not None else None


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
            targets = list(self._by_intersection.get(intersection_id, set()))

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


def load_intersection_lanes(intersection_id: int) -> List[Dict[str, Any]]:
    """Load lane definitions from dbo.intersection_lanes for one intersection."""
    conn = _open_db_connection()
    if conn is None:
        return []

    try:
        cursor = conn.cursor()
        cursor.execute(
            "SELECT lane_id, camera_index, direction, description "
            "FROM dbo.intersection_lanes "
            "WHERE intersection_id = ? "
            "ORDER BY camera_index, lane_id",
            (intersection_id,),
        )
        rows = cursor.fetchall()

        result: List[Dict[str, Any]] = []
        for row in rows:
            result.append(
                {
                    "lane_id": int(row[0]),
                    "camera_index": int(row[1]),
                    "direction": str(row[2]),
                    "description": str(row[3]) if row[3] is not None else None,
                }
            )
        return result
    except Exception:
        return []
    finally:
        if conn is not None:
            try:
                conn.close()
            except Exception:
                pass


def load_intersection_conflicts(intersection_id: int) -> List[Dict[str, Any]]:
    """Load lane conflict rows from dbo.lane_conflicts for one intersection."""
    conn = _open_db_connection()
    if conn is None:
        return []

    try:
        cursor = conn.cursor()
        cursor.execute(
            "SELECT conflict_id, lane_id_1, lane_id_2, conflict_type, created_at "
            "FROM dbo.lane_conflicts "
            "WHERE intersection_id = ? "
            "ORDER BY created_at DESC, conflict_id DESC",
            (intersection_id,),
        )
        rows = cursor.fetchall()

        result: List[Dict[str, Any]] = []
        for row in rows:
            result.append(
                {
                    "conflict_id": int(row[0]),
                    "lane_id_1": int(row[1]),
                    "lane_id_2": int(row[2]),
                    "conflict_type": str(row[3]) if row[3] is not None else "crossing",
                    "created_at": str(row[4]) if row[4] is not None else None,
                }
            )
        return result
    except Exception:
        return []
    finally:
        if conn is not None:
            try:
                conn.close()
            except Exception:
                pass


def build_phase_options_for_intersection(intersection_id: int) -> List[Dict[str, Any]]:
    """Build manual phase options dynamically for one intersection.

    Strategy (server-side, deterministic):
    - Primary groups: even lane ids => phase 0, odd lane ids => phase 1
    - Additional single-lane fallback groups get phase ids 2..N (if needed)
    """
    lanes = load_intersection_lanes(intersection_id)
    lane_ids = [int(item["lane_id"]) for item in lanes if item.get("lane_id") is not None]
    lane_ids = sorted(set(lane_ids))

    phase_options: List[Dict[str, Any]] = []
    even_lanes = [lane_id for lane_id in lane_ids if lane_id % 2 == 0]
    odd_lanes = [lane_id for lane_id in lane_ids if lane_id % 2 != 0]

    if even_lanes:
        phase_options.append(
            {
                "phase_id": 0,
                "green_lanes": even_lanes,
                "label": "Phase0",
                "source": "server_generated_even_odd",
            }
        )

    if odd_lanes:
        phase_options.append(
            {
                "phase_id": 1,
                "green_lanes": odd_lanes,
                "label": "Phase1",
                "source": "server_generated_even_odd",
            }
        )

    if not phase_options:
        # No lanes in DB; fallback to classic fixed phases so admin UI remains usable.
        for phase_id in (0, 1):
            phase_options.append(
                {
                    "phase_id": phase_id,
                    "green_lanes": [],
                    "label": f"Phase{phase_id}",
                    "source": "fallback_no_lanes",
                }
            )
        return phase_options

    # If parity split produced only one group, provide single-lane options as fallback.
    if len(phase_options) == 1:
        next_phase_id = 2
        for lane_id in lane_ids:
            phase_options.append(
                {
                    "phase_id": next_phase_id,
                    "green_lanes": [lane_id],
                    "label": f"Phase{next_phase_id}",
                    "source": "server_generated_single_lane_fallback",
                }
            )
            next_phase_id += 1

    return phase_options


def _open_db_connection():
    """Best-effort DB connection resolver via db_intersections.get_connection()."""
    try:
        db_module = __import__("db_intersections")
    except Exception:
        return None

    get_connection = getattr(db_module, "get_connection", None)
    if not callable(get_connection):
        return None

    try:
        return get_connection()
    except Exception:
        return None


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
        "hardware": {
            "real_mode": is_real_hardware_mode(),
            "manual_emergency_enabled": not is_real_hardware_mode(),
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
            "hardware": {
                "real_mode": is_real_hardware_mode(),
                "manual_emergency_enabled": not is_real_hardware_mode(),
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
        "hardware": {
            "real_mode": is_real_hardware_mode(),
            "manual_emergency_enabled": not is_real_hardware_mode(),
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
    incoming_signal = state.emergency_signal

    # Validate only fresh incoming active emergency requests.
    if incoming_signal is not None and incoming_signal.active:
        signal_ok, reason = validate_emergency_signal(incoming_signal)
        if not signal_ok:
            raise HTTPException(status_code=401, detail=f"Emergency signal rejected: {reason}")
        _set_emergency_latch(state.intersection_id, incoming_signal)
    elif incoming_signal is not None and not incoming_signal.active:
        # Explicit clear from sender/dashboard/hardware.
        _clear_emergency_latch(state.intersection_id)

    # If incoming state has no emergency payload, keep latched emergency for a short time.
    if incoming_signal is None:
        latched = _get_latched_emergency(state.intersection_id)
        if latched is not None:
            state.emergency_signal = latched

    state_store[state.intersection_id] = state
    action = decide_action(state)
    # ברירת מחדל: החלטת שרת, עד ש-C++ ידווח פעולה עדכנית.
    # אם התקבלה החלטת C++ לאחרונה - שומרים עליה כדי למנוע "קפיצות" תצוגה.
    now = time.time()
    last_source = action_source_store.get(state.intersection_id)
    last_updated_at = action_updated_at_store.get(state.intersection_id, 0.0)
    cpp_recent = last_source == "cpp_controller" and (now - last_updated_at) <= CPP_ACTION_STICKY_SEC
    manual_recent = last_source == "manual_dashboard" and (now - last_updated_at) <= MANUAL_ACTION_STICKY_SEC

    emergency_active = bool(state.emergency_signal and state.emergency_signal.active)
    if emergency_active:
        # Immediate preemption: emergency should always override sticky normal actions.
        action_store[state.intersection_id] = action
        action_source_store[state.intersection_id] = "emergency_preempt"
        action_updated_at_store[state.intersection_id] = now
    elif not cpp_recent and not manual_recent:
        action_store[state.intersection_id] = action
        action_source_store[state.intersection_id] = "server_fallback"
        action_updated_at_store[state.intersection_id] = now
    else:
        action = action_store.get(state.intersection_id, action)

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


@app.get("/intersection/{intersection_id}/lanes")
def get_intersection_lanes(intersection_id: int):
    """Return lane direction rows from dbo.intersection_lanes.

    Response format:
    [{lane_id, camera_index, direction, description}]
    """
    lanes = load_intersection_lanes(intersection_id)
    if not lanes:
        if fetch_intersection_by_id is not None:
            try:
                row = fetch_intersection_by_id(intersection_id)
                if row is None:
                    raise HTTPException(status_code=404, detail="Intersection not found")
            except HTTPException:
                raise
            except Exception:
                pass
    return lanes


@app.get("/intersection/{intersection_id}/conflicts")
def get_intersection_conflicts(intersection_id: int):
    """Return non-admin conflict pairs for one intersection.

    Response includes both a compact pair list (for C++) and detailed rows.
    """
    if fetch_intersection_by_id is not None:
        try:
            row = fetch_intersection_by_id(intersection_id)
            if row is None:
                raise HTTPException(status_code=404, detail="Intersection not found")
        except HTTPException:
            raise
        except Exception:
            pass

    rows = load_intersection_conflicts(intersection_id)
    pair_list = [[int(item["lane_id_1"]), int(item["lane_id_2"])] for item in rows]

    return {
        "status": "success",
        "intersection_id": intersection_id,
        "count": len(pair_list),
        "conflicts": pair_list,
        "conflict_rows": rows,
    }


@app.get("/intersection/{intersection_id}/layout")
def get_intersection_layout(intersection_id: int):
    """Return intersection directional layout derived from DB neighbor directions."""

    db_row = None
    if fetch_intersection_by_id is not None:
        try:
            db_row = fetch_intersection_by_id(intersection_id)
        except Exception:
            db_row = None

    state = state_store.get(intersection_id)
    num_lanes = 4
    if state is not None:
        num_lanes = max(1, int(state.num_lanes))
    elif db_row is not None:
        try:
            num_lanes = max(1, int(db_row.get("num_cameras") or 4))
        except Exception:
            num_lanes = 4

    neighbors_raw: List[Dict[str, Any]] = []
    if fetch_neighbors is not None:
        try:
            neighbors_raw = fetch_neighbors(intersection_id)
        except Exception:
            neighbors_raw = []

    def _normalize_direction(value: Any) -> Optional[str]:
        if value is None:
            return None
        raw = str(value).strip().upper()
        mapping = {
            "N": "N",
            "NORTH": "N",
            "S": "S",
            "SOUTH": "S",
            "E": "E",
            "EAST": "E",
            "W": "W",
            "WEST": "W",
        }
        return mapping.get(raw)

    ordered_neighbors: List[Dict[str, Any]] = []
    for n in neighbors_raw:
        direction = _normalize_direction(n.get("direction_from"))
        ordered_neighbors.append(
            {
                "adjacent_intersection_id": n.get("adjacent_intersection_id"),
                "adjacent_name": n.get("adjacent_name"),
                "direction_from": direction,
                "distance_m": n.get("distance_m"),
            }
        )

    preferred_order = ["N", "E", "S", "W"]
    existing_dirs = [n["direction_from"] for n in ordered_neighbors if n.get("direction_from") in preferred_order]

    lane_directions: List[str] = []
    for d in preferred_order:
        if d in existing_dirs:
            lane_directions.append(d)
    for d in preferred_order:
        if len(lane_directions) >= num_lanes:
            break
        if d not in lane_directions:
            lane_directions.append(d)

    while len(lane_directions) < num_lanes:
        lane_directions.append(f"L{len(lane_directions)}")

    return {
        "intersection_id": intersection_id,
        "num_lanes": num_lanes,
        "lane_directions": lane_directions[:num_lanes],
        "neighbors": ordered_neighbors,
    }


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
async def submit_controller_action(
    intersection_id: int,
    update: ControllerActionUpdate,
    credentials: Optional[HTTPAuthorizationCredentials] = Depends(security),
):
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
    reason_text = (update.reason or "selected_by_cpp_controller").strip().lower()
    source = "manual_dashboard" if "manual" in reason_text or "dashboard" in reason_text else "cpp_controller"

    if source == "manual_dashboard":
        if not credentials:
            raise HTTPException(status_code=403, detail="Manual control requires admin authentication")
        verify_admin_token(credentials)

    action_store[intersection_id] = action
    action_source_store[intersection_id] = source
    action_updated_at_store[intersection_id] = time.time()

    await LIVE_UPDATES.broadcast(
        event="action_updated",
        intersection_id=intersection_id,
        payload={
            "action": action.model_dump(),
            "source": source,
        },
    )

    return action


@app.post("/intersection/{intersection_id}/simulate-emergency", response_model=ActionResponse)
async def simulate_emergency(
    intersection_id: int,
    payload: SimulatedEmergencyRequest,
    admin: Dict[str, Any] = Depends(verify_admin_token),
):
    """
    Dashboard-only emergency trigger for simulation mode.
    Automatically disabled when real hardware mode is enabled.
    """
    if is_real_hardware_mode():
        raise HTTPException(status_code=403, detail="Manual dashboard emergency disabled in real hardware mode")

    state = state_store.get(intersection_id)
    if state is None:
        state = IntersectionState(
            intersection_id=intersection_id,
            num_lanes=max(payload.lane_id + 1, 4),
            timestamp=time.time(),
            lanes=[
                LaneState(lane_id=i, vehicle_count=0, density_pct=0.0, waiting_time_sec=0.0)
                for i in range(max(payload.lane_id + 1, 4))
            ],
            emergency_signal=None,
            neighbor_states={},
        )

    lane_id = payload.lane_id
    if lane_id < 0 or lane_id >= state.num_lanes:
        raise HTTPException(status_code=400, detail=f"lane_id out of range (0..{state.num_lanes - 1})")

    secret = EMERGENCY_KEYS.get(payload.vehicle_id) or EMERGENCY_KEYS.get("*")
    if not secret:
        raise HTTPException(status_code=400, detail="Unknown emergency vehicle_id")

    ts = time.time()
    signal = EmergencySignal(
        active=True,
        lane_id=lane_id,
        vehicle_id=payload.vehicle_id,
        timestamp=ts,
    )
    canonical = _canonical_emergency_payload(signal)
    signal.signature = hmac.new(secret.encode("utf-8"), canonical.encode("utf-8"), hashlib.sha256).hexdigest()

    state.emergency_signal = signal
    state.timestamp = ts

    signal_ok, reason = validate_emergency_signal(state.emergency_signal)
    if not signal_ok:
        raise HTTPException(status_code=401, detail=f"Emergency signal rejected: {reason}")

    _set_emergency_latch(intersection_id, signal)

    state_store[intersection_id] = state
    action = decide_action(state)
    action_store[intersection_id] = action
    action_source_store[intersection_id] = "manual_dashboard"
    action_updated_at_store[intersection_id] = time.time()

    await LIVE_UPDATES.broadcast(
        event="state_updated",
        intersection_id=intersection_id,
        payload={
            "state": state.model_dump(),
            "action": action.model_dump(),
        },
    )

    return action


@app.post("/intersection/{intersection_id}/clear-emergency", response_model=ActionResponse)
async def clear_emergency(
    intersection_id: int,
    admin: Dict[str, Any] = Depends(verify_admin_token),
):
    if is_real_hardware_mode():
        raise HTTPException(status_code=403, detail="Manual dashboard emergency disabled in real hardware mode")

    state = state_store.get(intersection_id)
    if state is None:
        raise HTTPException(status_code=404, detail="Intersection state not found")

    _clear_emergency_latch(intersection_id)

    state.emergency_signal = EmergencySignal(active=False)
    state.timestamp = time.time()

    action = decide_action(state)
    action_store[intersection_id] = action
    action_source_store[intersection_id] = "manual_dashboard"
    action_updated_at_store[intersection_id] = time.time()

    await LIVE_UPDATES.broadcast(
        event="state_updated",
        intersection_id=intersection_id,
        payload={
            "state": state.model_dump(),
            "action": action.model_dump(),
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

# ══════════════════════════════════════════════════════
# Admin Management Endpoints
# ══════════════════════════════════════════════════════

@app.post("/admin/login", response_model=AdminTokenResponse)
def admin_login(request: AdminLoginRequest):
    """Admin login endpoint - authenticate and get JWT token."""
    
    # Validate username exists
    if request.username not in ADMIN_CREDENTIALS:
        logger.warning(f"Failed login attempt for username: {request.username}")
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid credentials",
        )
    
    # Verify password
    password_hash = hashlib.sha256(request.password.encode()).hexdigest()
    if password_hash != ADMIN_CREDENTIALS[request.username]:
        logger.warning(f"Failed login attempt for username: {request.username}")
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid credentials",
        )
    
    # Create token
    token = create_admin_token(request.username)
    logger.info(f"Admin login successful for user: {request.username}")
    
    return AdminTokenResponse(
        access_token=token,
        expires_in=ADMIN_JWT_EXPIRATION_MINUTES * 60,
    )


@app.post("/admin/intersection/create")
def admin_create_intersection(
    request: IntersectionCreateRequest,
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    """Create a new intersection with validation."""
    
    try:
        # Validate all input
        if validate_intersection_data is not None:
            validated_data = validate_intersection_data({
                "code": request.code,
                "name": request.name,
                "latitude": request.latitude,
                "longitude": request.longitude,
                "num_cameras": request.num_cameras,
                "city": request.city,
                "region": request.region,
                "description": request.description,
            })
        else:
            validated_data = {
                "code": request.code,
                "name": request.name,
                "latitude": request.latitude,
                "longitude": request.longitude,
                "num_cameras": request.num_cameras,
                "city": request.city,
                "region": request.region,
                "description": request.description,
            }
        
        # Try to insert into database
        if db_insert_intersection is not None:
            try:
                existing = load_intersections()
                if any(item.code == validated_data["code"] for item in existing):
                    raise HTTPException(
                        status_code=status.HTTP_409_CONFLICT,
                        detail=f"Intersection code {validated_data['code']} already exists",
                    )

                db_insert_intersection(
                    code=validated_data['code'],
                    name=validated_data['name'],
                    latitude=validated_data['latitude'],
                    longitude=validated_data['longitude'],
                    num_cameras=validated_data['num_cameras'],
                    city=validated_data.get('city'),
                    region=validated_data.get('region'),
                    description=validated_data.get('description'),
                )
                logger.info(f"Admin {admin['username']} created intersection: {validated_data['code']}")
                
                return {
                    "status": "success",
                    "message": f"Intersection {validated_data['code']} created successfully",
                    "data": validated_data,
                }
            except Exception as e:
                logger.error(f"Database error creating intersection: {e}")
                raise HTTPException(
                    status_code=status.HTTP_400_BAD_REQUEST,
                    detail=f"Database error: {str(e)}",
                )
        else:
            logger.warning("Database module not available for intersection creation")
            raise HTTPException(
                status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
                detail="Database service not available",
            )
    
    except ValidationError as e:
        logger.warning(f"Validation error in intersection creation: {e}")
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail=str(e),
        )


@app.put("/admin/intersection/{intersection_id}")
def admin_update_intersection(
    intersection_id: int,
    request: IntersectionUpdateRequest,
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    """Update an existing intersection."""
    
    try:
        # Verify intersection exists
        if fetch_intersection_by_id is None:
            raise HTTPException(
                status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
                detail="Database service not available",
            )
        
        existing = fetch_intersection_by_id(intersection_id)
        if not existing:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"Intersection {intersection_id} not found",
            )
        
        # Validate update fields
        update_data = {}
        
        if request.name is not None:
            if validate_intersection_name is not None:
                update_data['name'] = validate_intersection_name(request.name)
            else:
                update_data['name'] = request.name
        
        if request.num_cameras is not None:
            if validate_num_cameras is not None:
                update_data['num_cameras'] = validate_num_cameras(request.num_cameras)
            else:
                update_data['num_cameras'] = request.num_cameras
        
        if request.city is not None:
            update_data['city'] = validate_city(request.city) if validate_city is not None else request.city
        
        if request.region is not None:
            update_data['region'] = validate_region(request.region) if validate_region is not None else request.region
        
        if request.description is not None:
            update_data['description'] = (
                validate_description(request.description)
                if validate_description is not None
                else request.description
            )
        
        if not update_data:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail="No updatable fields provided",
            )

        # Update in database
        if update_intersection is None:
            raise HTTPException(
                status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
                detail="Database update service not available",
            )

        update_intersection(intersection_id, update_data)
        
        logger.info(f"Admin {admin['username']} updated intersection {intersection_id}")
        
        return {
            "status": "success",
            "message": f"Intersection {intersection_id} updated successfully",
            "data": update_data,
        }
    
    except ValidationError as e:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail=str(e),
        )
    except Exception as e:
        logger.error(f"Error updating intersection: {e}")
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=str(e),
        )


@app.get("/admin/intersection/{intersection_id}")
def admin_get_intersection(
    intersection_id: int,
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    """Get detailed intersection information including neighbors."""
    
    try:
        if fetch_intersection_by_id is None:
            raise HTTPException(
                status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
                detail="Database service not available",
            )
        
        intersection = fetch_intersection_by_id(intersection_id)
        if not intersection:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"Intersection {intersection_id} not found",
            )
        
        # Get neighbors
        neighbors = []
        if fetch_neighbors is not None:
            try:
                neighbors = fetch_neighbors(intersection_id)
            except Exception as e:
                logger.warning(f"Could not fetch neighbors: {e}")

        lanes = load_intersection_lanes(intersection_id)
        
        # Get current state
        current_state = state_store.get(intersection_id)
        current_action = action_store.get(intersection_id)
        
        return {
            "status": "success",
            "intersection": intersection,
            "neighbors": neighbors,
            "lanes": lanes,
            "current_state": current_state.model_dump() if current_state else None,
            "current_action": current_action.model_dump() if current_action else None,
        }
    
    except HTTPException:
        raise
    except Exception as e:
        logger.error(f"Error getting intersection: {e}")
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=str(e),
        )


@app.get("/admin/intersection/{intersection_id}/phase-options")
def admin_get_intersection_phase_options(
    intersection_id: int,
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    """Return dynamic phase options for manual control UI."""
    if fetch_intersection_by_id is not None:
        row = fetch_intersection_by_id(intersection_id)
        if row is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Intersection not found")

    options = build_phase_options_for_intersection(intersection_id)
    return {
        "status": "success",
        "intersection_id": intersection_id,
        "count": len(options),
        "phase_options": options,
    }


_VALID_LANE_DIRECTIONS = {"N", "S", "E", "W", "NE", "NW", "SE", "SW"}


def _normalize_lane_direction(value: str) -> str:
    direction = str(value or "").strip().upper()
    if direction not in _VALID_LANE_DIRECTIONS:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="direction must be one of N,S,E,W,NE,NW,SE,SW",
        )
    return direction


@app.get("/admin/intersection/{intersection_id}/lanes")
def admin_get_intersection_lanes(
    intersection_id: int,
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    if fetch_intersection_by_id is not None:
        row = fetch_intersection_by_id(intersection_id)
        if row is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Intersection not found")

    lanes = load_intersection_lanes(intersection_id)
    return {
        "status": "success",
        "intersection_id": intersection_id,
        "count": len(lanes),
        "lanes": lanes,
    }


@app.post("/admin/intersection/{intersection_id}/lanes")
def admin_create_intersection_lane(
    intersection_id: int,
    request: LaneCreateRequest,
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    if fetch_intersection_by_id is not None:
        row = fetch_intersection_by_id(intersection_id)
        if row is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Intersection not found")

    direction = _normalize_lane_direction(request.direction)
    conn = _open_db_connection()
    if conn is None:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="Database service not available")

    try:
        cursor = conn.cursor()
        cursor.execute(
            "INSERT INTO dbo.intersection_lanes (intersection_id, camera_index, direction, description) "
            "VALUES (?, ?, ?, ?)",
            (
                intersection_id,
                int(request.camera_index),
                direction,
                request.description,
            ),
        )
        conn.commit()
    except Exception as e:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=f"Failed creating lane: {e}")
    finally:
        try:
            conn.close()
        except Exception:
            pass

    logger.info(f"Admin {admin['username']} created lane for intersection {intersection_id}")
    return {
        "status": "success",
        "message": "Lane created successfully",
        "lanes": load_intersection_lanes(intersection_id),
    }


@app.put("/admin/intersection/{intersection_id}/lanes/{lane_id}")
def admin_update_intersection_lane(
    intersection_id: int,
    lane_id: int,
    request: LaneUpdateRequest,
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    updates: List[str] = []
    values: List[Any] = []

    if request.direction is not None:
        updates.append("direction = ?")
        values.append(_normalize_lane_direction(request.direction))

    if request.description is not None:
        updates.append("description = ?")
        values.append(request.description)

    if not updates:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="No fields to update")

    conn = _open_db_connection()
    if conn is None:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="Database service not available")

    try:
        cursor = conn.cursor()
        cursor.execute(
            "SELECT 1 FROM dbo.intersection_lanes WHERE lane_id = ? AND intersection_id = ?",
            (lane_id, intersection_id),
        )
        if cursor.fetchone() is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Lane not found")

        query = (
            f"UPDATE dbo.intersection_lanes SET {', '.join(updates)} "
            "WHERE lane_id = ? AND intersection_id = ?"
        )
        values.extend([lane_id, intersection_id])
        cursor.execute(query, tuple(values))
        conn.commit()
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=f"Failed updating lane: {e}")
    finally:
        try:
            conn.close()
        except Exception:
            pass

    logger.info(f"Admin {admin['username']} updated lane {lane_id} for intersection {intersection_id}")
    return {
        "status": "success",
        "message": "Lane updated successfully",
        "lanes": load_intersection_lanes(intersection_id),
    }


@app.delete("/admin/intersection/{intersection_id}/lanes/{lane_id}")
def admin_delete_intersection_lane(
    intersection_id: int,
    lane_id: int,
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    conn = _open_db_connection()
    if conn is None:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="Database service not available")

    try:
        cursor = conn.cursor()
        cursor.execute(
            "SELECT 1 FROM dbo.intersection_lanes WHERE lane_id = ? AND intersection_id = ?",
            (lane_id, intersection_id),
        )
        if cursor.fetchone() is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Lane not found")

        cursor.execute(
            "DELETE FROM dbo.intersection_lanes WHERE lane_id = ? AND intersection_id = ?",
            (lane_id, intersection_id),
        )
        conn.commit()
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=f"Failed deleting lane: {e}")
    finally:
        try:
            conn.close()
        except Exception:
            pass

    logger.info(f"Admin {admin['username']} deleted lane {lane_id} from intersection {intersection_id}")
    return {
        "status": "success",
        "message": "Lane deleted successfully",
        "lanes": load_intersection_lanes(intersection_id),
    }


@app.get("/admin/intersections")
def admin_list_intersections(
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    """List all intersections with their current state."""
    
    try:
        intersections = load_intersections()
        
        # Enrich with current state
        result = []
        for intersection in intersections:
            state = state_store.get(intersection.id)
            action = action_store.get(intersection.id)
            
            result.append({
                "intersection": intersection.model_dump(),
                "current_state": state.model_dump() if state else None,
                "current_action": action.model_dump() if action else None,
            })
        
        return {
            "status": "success",
            "count": len(result),
            "intersections": result,
        }
    
    except Exception as e:
        logger.error(f"Error listing intersections: {e}")
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=str(e),
        )


@app.post("/admin/manual-control")
async def admin_manual_control(
    request: ManualControlRequest,
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    """Manually set traffic signal phase for an intersection."""
    
    try:
        if validate_phase_id is not None:
            phase_id = validate_phase_id(request.phase_id)
        else:
            phase_id = request.phase_id
        
        # Create action
        action = ActionResponse(
            action=f"Phase{phase_id}",
            reason=f"admin_manual_control_by_{admin['username']}",
            intersection_id=request.intersection_id,
            phase_id=phase_id,
        )
        
        # Store action
        action_store[request.intersection_id] = action
        
        # Broadcast update
        await LIVE_UPDATES.broadcast(
            "manual_control",
            {
                "intersection_id": request.intersection_id,
                "action": action.model_dump(),
                "timestamp": time.time(),
            },
            request.intersection_id
        )
        
        logger.info(
            f"Admin {admin['username']} set manual control for intersection "
            f"{request.intersection_id} to phase {phase_id}"
        )
        
        return {
            "status": "success",
            "message": f"Manual control applied to intersection {request.intersection_id}",
            "action": action.model_dump(),
        }
    
    except ValidationError as e:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail=str(e),
        )
    except Exception as e:
        logger.error(f"Error in manual control: {e}")
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=str(e),
        )


@app.get("/admin/auth/verify")
def admin_verify_token(admin: Dict[str, Any] = Depends(verify_admin_token)):
    return {
        "status": "success",
        "username": admin["username"],
        "message": "token_valid",
    }


@app.get("/admin/intersection/{intersection_id}/neighbors")
def admin_get_neighbors_simulation(
    intersection_id: int,
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    """Get neighbor information for simulation visualization."""
    
    try:
        # Get neighbors from topology
        neighbor_topology = load_neighbor_topology()
        neighbor_ids = neighbor_topology.get(intersection_id, [])
        
        result = []
        for neighbor_id in neighbor_ids:
            neighbor_state = state_store.get(neighbor_id)
            neighbor_action = action_store.get(neighbor_id)
            
            result.append({
                "intersection_id": neighbor_id,
                "state": neighbor_state.model_dump() if neighbor_state else None,
                "action": neighbor_action.model_dump() if neighbor_action else None,
            })
        
        return {
            "status": "success",
            "intersection_id": intersection_id,
            "neighbor_count": len(result),
            "neighbors": result,
        }
    
    except Exception as e:
        logger.error(f"Error getting neighbors: {e}")
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=str(e),
        )


# ═══════════════════════════════════════════════════════════════════
# LANE CONFLICT MANAGEMENT ENDPOINTS
# ═══════════════════════════════════════════════════════════════════

@app.get("/admin/intersection/{intersection_id}/conflicts")
def admin_get_lane_conflicts(
    intersection_id: int,
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    """Get all lane conflicts for a specific intersection."""
    if fetch_intersection_by_id is not None:
        row = fetch_intersection_by_id(intersection_id)
        if row is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Intersection not found")

    conn = _open_db_connection()
    if conn is None:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="Database service not available")

    try:
        cursor = conn.cursor()
        cursor.execute(
            """
            SELECT conflict_id, intersection_id, lane_id_1, lane_id_2, conflict_type, created_at
            FROM dbo.lane_conflicts
            WHERE intersection_id = ?
            ORDER BY created_at DESC
            """,
            (intersection_id,),
        )
        rows = cursor.fetchall()
        
        conflicts = []
        for row in rows:
            conflicts.append({
                "conflict_id": row[0],
                "intersection_id": row[1],
                "lane_id_1": row[2],
                "lane_id_2": row[3],
                "conflict_type": row[4],
                "created_at": str(row[5]) if row[5] else None,
            })
        
        return {
            "status": "success",
            "intersection_id": intersection_id,
            "count": len(conflicts),
            "conflicts": conflicts,
        }
    except Exception as e:
        logger.error(f"Error fetching conflicts: {e}")
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=f"Failed fetching conflicts: {e}")
    finally:
        try:
            conn.close()
        except Exception:
            pass


@app.post("/admin/intersection/{intersection_id}/conflicts")
def admin_create_lane_conflict(
    intersection_id: int,
    request: LaneConflictCreateRequest,
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    """Create a new lane conflict for an intersection."""
    if fetch_intersection_by_id is not None:
        row = fetch_intersection_by_id(intersection_id)
        if row is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Intersection not found")

    # Validation: cannot conflict with itself
    if request.lane_id_1 == request.lane_id_2:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="A lane cannot conflict with itself"
        )

    conn = _open_db_connection()
    if conn is None:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="Database service not available")

    try:
        cursor = conn.cursor()
        
        # Verify both lanes exist and belong to this intersection
        cursor.execute(
            "SELECT lane_id FROM dbo.intersection_lanes WHERE lane_id = ? AND intersection_id = ?",
            (request.lane_id_1, intersection_id)
        )
        if cursor.fetchone() is None:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"Lane {request.lane_id_1} not found in intersection {intersection_id}"
            )
        
        cursor.execute(
            "SELECT lane_id FROM dbo.intersection_lanes WHERE lane_id = ? AND intersection_id = ?",
            (request.lane_id_2, intersection_id)
        )
        if cursor.fetchone() is None:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"Lane {request.lane_id_2} not found in intersection {intersection_id}"
            )
        
        # Ensure consistent ordering: lane_id_1 < lane_id_2
        lane_1 = min(request.lane_id_1, request.lane_id_2)
        lane_2 = max(request.lane_id_1, request.lane_id_2)
        
        # Check for duplicate conflict
        cursor.execute(
            """
            SELECT conflict_id FROM dbo.lane_conflicts
            WHERE intersection_id = ? AND lane_id_1 = ? AND lane_id_2 = ?
            """,
            (intersection_id, lane_1, lane_2)
        )
        if cursor.fetchone() is not None:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail="This conflict pair already exists"
            )
        
        # Insert the conflict
        cursor.execute(
            """
            INSERT INTO dbo.lane_conflicts (intersection_id, lane_id_1, lane_id_2, conflict_type)
            VALUES (?, ?, ?, ?)
            """,
            (intersection_id, lane_1, lane_2, request.conflict_type or "crossing")
        )
        conn.commit()
        
        logger.info(
            f"Admin {admin['username']} created conflict between lanes {lane_1} and {lane_2} "
            f"in intersection {intersection_id}"
        )
        
        # Return updated conflicts list
        cursor.execute(
            """
            SELECT conflict_id, intersection_id, lane_id_1, lane_id_2, conflict_type, created_at
            FROM dbo.lane_conflicts
            WHERE intersection_id = ?
            ORDER BY created_at DESC
            """,
            (intersection_id,),
        )
        rows = cursor.fetchall()
        
        conflicts = []
        for row in rows:
            conflicts.append({
                "conflict_id": row[0],
                "intersection_id": row[1],
                "lane_id_1": row[2],
                "lane_id_2": row[3],
                "conflict_type": row[4],
                "created_at": str(row[5]) if row[5] else None,
            })
        
        return {
            "status": "success",
            "message": "Lane conflict created successfully",
            "intersection_id": intersection_id,
            "count": len(conflicts),
            "conflicts": conflicts,
        }
    
    except HTTPException:
        raise
    except Exception as e:
        logger.error(f"Error creating conflict: {e}")
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=f"Failed creating conflict: {e}"
        )
    finally:
        try:
            conn.close()
        except Exception:
            pass


@app.delete("/admin/intersection/{intersection_id}/conflicts/{conflict_id}")
def admin_delete_lane_conflict(
    intersection_id: int,
    conflict_id: int,
    admin: Dict[str, Any] = Depends(verify_admin_token)
):
    """Delete a lane conflict from an intersection."""
    if fetch_intersection_by_id is not None:
        row = fetch_intersection_by_id(intersection_id)
        if row is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Intersection not found")

    conn = _open_db_connection()
    if conn is None:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="Database service not available")

    try:
        cursor = conn.cursor()
        
        # Verify conflict exists and belongs to this intersection
        cursor.execute(
            "SELECT conflict_id FROM dbo.lane_conflicts WHERE conflict_id = ? AND intersection_id = ?",
            (conflict_id, intersection_id)
        )
        if cursor.fetchone() is None:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"Conflict {conflict_id} not found in intersection {intersection_id}"
            )
        
        # Delete the conflict
        cursor.execute(
            "DELETE FROM dbo.lane_conflicts WHERE conflict_id = ? AND intersection_id = ?",
            (conflict_id, intersection_id)
        )
        conn.commit()
        
        logger.info(
            f"Admin {admin['username']} deleted conflict {conflict_id} from intersection {intersection_id}"
        )
        
        # Return updated conflicts list
        cursor.execute(
            """
            SELECT conflict_id, intersection_id, lane_id_1, lane_id_2, conflict_type, created_at
            FROM dbo.lane_conflicts
            WHERE intersection_id = ?
            ORDER BY created_at DESC
            """,
            (intersection_id,),
        )
        rows = cursor.fetchall()
        
        conflicts = []
        for row in rows:
            conflicts.append({
                "conflict_id": row[0],
                "intersection_id": row[1],
                "lane_id_1": row[2],
                "lane_id_2": row[3],
                "conflict_type": row[4],
                "created_at": str(row[5]) if row[5] else None,
            })
        
        return {
            "status": "success",
            "message": "Lane conflict deleted successfully",
            "intersection_id": intersection_id,
            "count": len(conflicts),
            "conflicts": conflicts,
        }
    
    except HTTPException:
        raise
    except Exception as e:
        logger.error(f"Error deleting conflict: {e}")
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=f"Failed deleting conflict: {e}"
        )
    finally:
        try:
            conn.close()
        except Exception:
            pass