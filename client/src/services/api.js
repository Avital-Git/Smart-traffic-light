const API_BASE = 'http://127.0.0.1:8000';
const WS_BASE = API_BASE.replace(/^http/i, 'ws');

const mockIntersections = [
  { id: 1, code: 'J-001', name: 'צומת מרכזי', city: 'באר שבע' },
  { id: 2, code: 'J-002', name: 'צומת האוניברסיטה', city: 'באר שבע' },
  { id: 3, code: 'J-003', name: 'צומת בית חולים', city: 'באר שבע' },
  { id: 4, code: 'J-004', name: 'צומת דרומי', city: 'באר שבע' }
];

function laneToSignalColor(currentPhase, laneId) {
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

export async function getNetworkMetrics() {
  return safeFetch(`${API_BASE}/metrics/summary`, () => ({
    timestamp: Date.now() / 1000,
    intersection_count: 0,
    total_network_queue: 0,
    avg_network_waiting_sec: 0,
    intersections: [],
    security: {
      emergency_keys_source: 'fallback',
      neighbor_message_auth_source: 'fallback'
    }
  }));
}

export async function getIntersectionStatus(intersectionId) {
  const [state, action] = await Promise.all([
    safeFetch(`${API_BASE}/intersection/${intersectionId}`, () => null),
    safeFetch(`${API_BASE}/intersection/${intersectionId}/action`, () => null)
  ]);

  if (!state) {
    return buildMockStatus(intersectionId);
  }

  return mapStateToStatus(state, action, intersectionId);
}

function mapStateToStatus(state, action, intersectionId) {
  const resolvedIntersectionId = state?.intersection_id ?? intersectionId;
  const currentPhase = action?.action || 'Hold';
  const lanes = state?.lanes || [];
  const totalQueue = lanes.reduce((sum, lane) => sum + (lane.vehicle_count || 0), 0);
  const avgWaitSec = lanes.length > 0
    ? Math.round(lanes.reduce((sum, lane) => sum + (lane.waiting_time_sec || 0), 0) / lanes.length)
    : 0;

  return {
    intersectionId: resolvedIntersectionId,
    currentPhase,
    congestionLevel: congestionLevel(totalQueue),
    totalQueue,
    avgWaitSec,
    manualOverrideEnabled: false,
    emergencyActive: Boolean(state.emergency_signal?.active),
    updatedAt: new Date((state.timestamp || Date.now() / 1000) * 1000).toLocaleString('he-IL'),
    signals: lanes.map((lane) => ({
      direction: `נתיב ${lane.lane_id}`,
      color: laneToSignalColor(currentPhase, lane.lane_id),
      queue: lane.vehicle_count,
      waitingSec: lane.waiting_time_sec
    })),
    chart: lanes.map((lane, index) => ({
      time: `נתיב ${lane.lane_id}`,
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
    lanes
  };
}

export function subscribeIntersectionUpdates(intersectionId, onUpdate) {
  if (!intersectionId || typeof onUpdate !== 'function') {
    return () => {};
  }

  const socket = new WebSocket(`${WS_BASE}/ws/intersection/${intersectionId}`);

  socket.onmessage = (event) => {
    try {
      const message = JSON.parse(event.data);
      const payload = message?.payload || {};

      if (message.event === 'state_updated' && payload.state) {
        onUpdate(mapStateToStatus(payload.state, payload.action, intersectionId), message);
        return;
      }

      if (message.event === 'welcome' && payload.state) {
        onUpdate(mapStateToStatus(payload.state, payload.action, intersectionId), message);
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
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });

    return { ok: response.ok };
  } catch {
    return { ok: false };
  }
}
