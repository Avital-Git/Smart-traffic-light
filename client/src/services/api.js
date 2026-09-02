const API_BASE = 'http://127.0.0.1:8000';
const WS_BASE = API_BASE.replace(/^http/i, 'ws');
const ADMIN_TOKEN_KEY = 'smart_traffic_admin_token';

// Module-level cache so layout is fetched exactly once per intersection ID.
// This prevents lane direction labels from flickering back to numbers on every
// polling cycle or WebSocket reconnect when a transient layout fetch fails.
const _layoutCache = new Map();

async function _fetchLayoutOnce(intersectionId) {
  if (_layoutCache.has(intersectionId)) {
    return _layoutCache.get(intersectionId);
  }
  try {
    const response = await fetch(`${API_BASE}/intersection/${intersectionId}/layout`);
    if (!response.ok) return null;
    const data = await response.json();
    if (data && Array.isArray(data.lane_directions)) {
      _layoutCache.set(intersectionId, data);
    }
    return data;
  } catch {
    return null;
  }
}

function authHeaders(base = {}) {
  const token = window.localStorage.getItem(ADMIN_TOKEN_KEY);
  return token
    ? { ...base, Authorization: `Bearer ${token}` }
    : base;
}

const mockIntersections = [
  { id: 1, code: 'J-001', name: 'צומת מרכזי', city: 'באר שבע' },
  { id: 2, code: 'J-002', name: 'צומת האוניברסיטה', city: 'באר שבע' },
  { id: 3, code: 'J-003', name: 'צומת בית חולים', city: 'באר שבע' },
  { id: 4, code: 'J-004', name: 'צומת דרומי', city: 'באר שבע' }
];

function laneToSignalColor(currentPhase, laneId, emergencyActive, emergencyLaneId) {
  // בחירום: רק נתיב החירום ומי שבאותה פאזה (לא סותרים) ירוק — כל השאר אדום
  if (emergencyActive && emergencyLaneId != null) {
    return (laneId % 2) === (emergencyLaneId % 2) ? 'GREEN' : 'RED';
  }

  if (currentPhase == null || currentPhase === 'Hold') {
    return 'RED';
  }

  const phaseMatch = /^Phase(\d+)$/.exec(currentPhase);
  if (!phaseMatch) {
    return 'RED';
  }

  const phaseId = Number(phaseMatch[1]);
  const lanePhase = laneId % 2 === 0 ? 0 : 1;
  return lanePhase === phaseId ? 'GREEN' : 'RED';
}

function congestionLevel(totalQueue) {
  if (totalQueue >= 55) return 'HIGH';
  if (totalQueue >= 30) return 'MEDIUM';
  return 'LOW';
}

function buildMockChart() {
  const now = Date.now();
  return Array.from({ length: 12 }).map((_, index) => {
    const time = new Date(now - (11 - index) * 60_000);
    return {
      time: time.toLocaleTimeString('he-IL', { hour: '2-digit', minute: '2-digit' }),
      queue: 20 + Math.floor(Math.random() * 35),
      avgWaitSec: 8 + Math.floor(Math.random() * 30)
    };
  });
}

function buildMockStatus(intersectionId) {
  const currentPhase = Math.random() > 0.5 ? 'Phase0' : 'Phase1';
  const emergencyActive = Math.random() > 0.8;
  const lanes = [
    { lane_id: 0, vehicle_count: 12, density_pct: 55, waiting_time_sec: 10 },
    { lane_id: 1, vehicle_count: 7, density_pct: 28, waiting_time_sec: 4 },
    { lane_id: 2, vehicle_count: 10, density_pct: 48, waiting_time_sec: 8 },
    { lane_id: 3, vehicle_count: 5, density_pct: 22, waiting_time_sec: 3 }
  ];
  const totalQueue = lanes.reduce((sum, lane) => sum + lane.vehicle_count, 0);

  return {
    intersectionId,
    currentPhase,
    congestionLevel: congestionLevel(totalQueue),
    totalQueue,
    avgWaitSec: Math.round(lanes.reduce((sum, lane) => sum + lane.waiting_time_sec, 0) / lanes.length),
    manualOverrideEnabled: false,
    emergencyActive,
    updatedAt: new Date().toLocaleString('he-IL'),
    signals: lanes.map((lane) => ({
      direction: `נתיב ${lane.lane_id}`,
      color: laneToSignalColor(currentPhase, lane.lane_id),
      queue: lane.vehicle_count,
      waitingSec: lane.waiting_time_sec
    })),
    chart: buildMockChart(),
    alerts: emergencyActive
      ? [{ id: 'alert-emergency', severity: 'high', message: 'זוהה רכב חירום בצומת', timestamp: new Date().toLocaleTimeString('he-IL') }]
      : [{ id: 'alert-ok', severity: 'low', message: 'אין התרעות פעילות כרגע', timestamp: new Date().toLocaleTimeString('he-IL') }],
    lanes
  };
}

async function safeFetch(url, fallbackFactory) {
  try {
    const response = await fetch(url);
    if (!response.ok) {
      return fallbackFactory();
    }
    return await response.json();
  } catch {
    return fallbackFactory();
  }
}

export async function getIntersections() {
  return safeFetch(`${API_BASE}/intersections`, () => mockIntersections);
}

export async function getSystemConfig() {
  return safeFetch(`${API_BASE}/config`, () => ({
    hardware: {
      real_mode: false,
      manual_emergency_enabled: true
    }
  }));
}

export async function getNetworkMetrics() {
  return safeFetch(`${API_BASE}/metrics/summary`, () => null);
}

export async function getIntersectionLayout(intersectionId) {
  // Use the module-level cache so callers always get the last known layout
  // and never degrade back to an empty array due to a transient fetch failure.
  if (_layoutCache.has(intersectionId)) {
    return _layoutCache.get(intersectionId);
  }
  const data = await safeFetch(`${API_BASE}/intersection/${intersectionId}/layout`, () => null);
  if (data && Array.isArray(data.lane_directions)) {
    _layoutCache.set(intersectionId, data);
  }
  return data;
}

export async function getIntersectionStatus(intersectionId) {
  const [state, action, layout] = await Promise.all([
    safeFetch(`${API_BASE}/intersection/${intersectionId}`, () => null),
    safeFetch(`${API_BASE}/intersection/${intersectionId}/action`, () => null),
    getIntersectionLayout(intersectionId)
  ]);

  if (!state) {
    return null;
  }

  return mapStateToStatus(state, action, intersectionId, layout);
}

function mapStateToStatus(state, action, intersectionId, layout) {
  const resolvedIntersectionId = state?.intersection_id ?? intersectionId;
  const currentPhase = action?.action || 'Hold';
  const lanes = state?.lanes || [];
  const totalQueue = lanes.reduce((sum, lane) => sum + (lane.vehicle_count || 0), 0);
  const avgWaitSec = lanes.length > 0
    ? Math.round(lanes.reduce((sum, lane) => sum + (lane.waiting_time_sec || 0), 0) / lanes.length)
    : 0;

  const laneDirections = Array.isArray(layout?.lane_directions)
    ? layout.lane_directions
    : [];

  // Translate a raw direction code to a Hebrew label.
  function translateDirection(d) {
    if (!d) return null;
    if (d === 'N')  return 'צפון';
    if (d === 'S')  return 'דרום';
    if (d === 'E')  return 'מזרח';
    if (d === 'W')  return 'מערב';
    if (d === 'NE') return 'צפון-מזרח';
    if (d === 'NW') return 'צפון-מערב';
    if (d === 'SE') return 'דרום-מזרח';
    if (d === 'SW') return 'דרום-מערב';
    return d; // return raw code if not one of the above
  }

  // Primary: use the `direction` field already stamped onto the lane object by
  // the C++ normalization pass (always canonical DB direction).
  // Fallback: look up by 0-based lane_id in the layout array.
  // This means lane names are stable even if the layout fetch is delayed or
  // transiently fails, because the state itself carries the direction.
  const directionLabel = (lane) => {
    // lane may be passed as the full object OR just the laneId (legacy callers).
    const laneObj  = (typeof lane === 'object' && lane !== null) ? lane : null;
    const laneId   = laneObj ? (laneObj.lane_id ?? lane) : lane;
    const fromState = laneObj?.direction ?? null;
    const fromLayout = laneDirections[laneId] ?? null;
    const code = fromState || fromLayout;
    return translateDirection(code) ?? `נתיב ${laneId}`;
  };

  return {
    intersectionId: resolvedIntersectionId,
    currentPhase,
    congestionLevel: congestionLevel(totalQueue),
    totalQueue,
    avgWaitSec,
    manualOverrideEnabled: false,
    emergencyActive: Boolean(state.emergency_signal?.active) || action?.reason === 'emergency_preempt' || action?.reason === 'emergency_preempt_gps', // emergency_preempt_gps = מקור GPS locate
    emergencyLaneId: state.emergency_signal?.lane_id ?? ((action?.reason === 'emergency_preempt' || action?.reason === 'emergency_preempt_gps') ? (action?.phase_id === 0 ? 0 : 1) : null), // נתיב חירום: מה-signal או מה-phase
    emergencyVehicleId: state.emergency_signal?.vehicle_id ?? null,
    updatedAt: new Date((state.timestamp || Date.now() / 1000) * 1000).toLocaleString('he-IL'),
    signals: lanes.map((lane) => ({
      direction: directionLabel(lane),
      color: laneToSignalColor(
        currentPhase,
        lane.lane_id,
        Boolean(state.emergency_signal?.active) || action?.reason === 'emergency_preempt' || action?.reason === 'emergency_preempt_gps', // מקור GPS — גם emergency_preempt_gps מציב אדום/ירוק
        state.emergency_signal?.lane_id ?? ((action?.reason === 'emergency_preempt' || action?.reason === 'emergency_preempt_gps') ? (action?.phase_id === 0 ? 0 : 1) : null) // צבע: נתיב חירום ירוק, שאר אדום
      ),
      queue: lane.vehicle_count,
      waitingSec: lane.waiting_time_sec
    })),
    chart: lanes.map((lane, index) => ({
      time: directionLabel(lane),
      queue: lane.vehicle_count,
      avgWaitSec: lane.waiting_time_sec,
      index
    })),
    alerts: Boolean(state.emergency_signal?.active)
      ? [{
          id: `emergency-${intersectionId}`,
          severity: 'high',
          message: `חירום פעיל בנתיב ${state.emergency_signal?.lane_id ?? '-'}`,
          timestamp: new Date().toLocaleTimeString('he-IL')
        }]
      : [{
          id: `normal-${intersectionId}`,
          severity: 'low',
          message: 'אין התרעות חירום פעילות',
          timestamp: new Date().toLocaleTimeString('he-IL')
        }],
    lanes,
    laneDirections,
    neighbors: Array.isArray(layout?.neighbors) ? layout.neighbors : []
  };
}

export function subscribeIntersectionUpdates(intersectionId, onUpdate) {
  if (!intersectionId || typeof onUpdate !== 'function') {
    return () => {};
  }

  // Use the module-level cache immediately (populated on first call to
  // getIntersectionLayout or _fetchLayoutOnce).  If it's not yet cached we
  // kick off the fetch — but even before it resolves, the state's own
  // `lane.direction` field provides direction labels without flickering.
  let cachedLayout = _layoutCache.get(intersectionId) ?? null;
  if (!cachedLayout) {
    _fetchLayoutOnce(intersectionId).then((layout) => {
      if (layout) cachedLayout = layout;
    });
  }

  const socket = new WebSocket(`${WS_BASE}/ws/intersection/${intersectionId}`);

  socket.onmessage = (event) => {
    try {
      const message = JSON.parse(event.data);
      const payload = message?.payload || {};

      // Guard against cross-intersection events that may arrive on shared broadcasts.
      if (
        message?.intersection_id != null &&
        Number(message.intersection_id) !== Number(intersectionId)
      ) {
        return;
      }

      if (message.event === 'state_updated' && payload.state) {
        onUpdate(mapStateToStatus(payload.state, payload.action, intersectionId, cachedLayout), message);
        return;
      }

      if (message.event === 'welcome' && payload.state) {
        onUpdate(mapStateToStatus(payload.state, payload.action, intersectionId, cachedLayout), message);
      }
    } catch {
      // Ignore malformed events
    }
  };

  const keepAlive = window.setInterval(() => {
    if (socket.readyState === WebSocket.OPEN) {
      socket.send('ping');
    }
  }, 15000);

  return () => {
    window.clearInterval(keepAlive);
    if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) {
      socket.close();
    }
  };
}

export function subscribeGlobalUpdates(onEvent) {
  if (typeof onEvent !== 'function') {
    return () => {};
  }

  const socket = new WebSocket(`${WS_BASE}/ws/updates`);

  socket.onmessage = (event) => {
    try {
      const message = JSON.parse(event.data);
      onEvent(message);
    } catch {
      // Ignore malformed events
    }
  };

  const keepAlive = window.setInterval(() => {
    if (socket.readyState === WebSocket.OPEN) {
      socket.send('ping');
    }
  }, 15000);

  return () => {
    window.clearInterval(keepAlive);
    if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) {
      socket.close();
    }
  };
}

export async function sendManualControl(intersectionId, action, enabled) {
  try {
    const body = {
      action,
      phase_id: /^Phase(\d+)$/.test(action) ? Number(action.replace('Phase', '')) : undefined,
      reason: enabled ? 'manual_override_from_dashboard' : 'dashboard_command'
    };

    const response = await fetch(`${API_BASE}/intersection/${intersectionId}/action`, {
      method: 'POST',
      headers: authHeaders({ 'Content-Type': 'application/json' }),
      body: JSON.stringify(body)
    });

    if (response.ok) {
      return { ok: true };
    }

    let detail = 'שליחת הפקודה נכשלה';
    try {
      const payload = await response.json();
      if (payload?.detail) {
        detail = String(payload.detail);
      }
    } catch {
      // ignore parse errors
    }

    return { ok: false, detail };
  } catch {
    return { ok: false, detail: 'שגיאת תקשורת לשרת' };
  }
}

export async function triggerEmergency(intersectionId, laneId = 0, vehicleId = 'AMB001') {
  try {
    const response = await fetch(`${API_BASE}/intersection/${intersectionId}/simulate-emergency`, {
      method: 'POST',
      headers: authHeaders({ 'Content-Type': 'application/json' }),
      body: JSON.stringify({ lane_id: laneId, vehicle_id: vehicleId })
    });

    if (response.ok) {
      return { ok: true };
    }

    let detail = 'שליחת חירום נכשלה';
    try {
      const payload = await response.json();
      if (payload?.detail) {
        detail = String(payload.detail);
      }
    } catch {
      // ignore parse errors
    }

    return { ok: false, detail };
  } catch {
    return { ok: false, detail: 'שגיאת תקשורת לשרת' };
  }
}

export async function clearEmergency(intersectionId) {
  try {
    const response = await fetch(`${API_BASE}/intersection/${intersectionId}/clear-emergency`, {
      method: 'POST',
      headers: authHeaders()
    });

    if (response.ok) {
      return { ok: true };
    }

    let detail = 'ניקוי חירום נכשל';
    try {
      const payload = await response.json();
      if (payload?.detail) {
        detail = String(payload.detail);
      }
    } catch {
      // ignore parse errors
    }

    return { ok: false, detail };
  } catch {
    return { ok: false, detail: 'שגיאת תקשורת לשרת' };
  }
}
