const API_BASE = 'http://127.0.0.1:8000';

const TOKEN_KEY = 'smart_traffic_admin_token';

function getToken() {
  return window.localStorage.getItem(TOKEN_KEY);
}

function setToken(token) {
  if (!token) {
    window.localStorage.removeItem(TOKEN_KEY);
    window.dispatchEvent(new Event('smart-traffic-admin-token-changed'));
    return;
  }
  window.localStorage.setItem(TOKEN_KEY, token);
  window.dispatchEvent(new Event('smart-traffic-admin-token-changed'));
}

function authHeaders() {
  const token = getToken();
  return {
    'Content-Type': 'application/json',
    ...(token ? { Authorization: `Bearer ${token}` } : {})
  };
}

async function parseResponse(response) {
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) {
    return {
      ok: false,
      status: response.status,
      detail: payload?.detail || payload?.message || 'Server error'
    };
  }
  return { ok: true, data: payload };
}

export function getAdminToken() {
  return getToken();
}

export function clearAdminToken() {
  setToken(null);
}

export async function adminLogin(username, password) {
  const response = await fetch(`${API_BASE}/admin/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username, password })
  });

  const parsed = await parseResponse(response);
  if (!parsed.ok) return parsed;

  setToken(parsed.data.access_token);
  return parsed;
}

export async function verifyAdminToken() {
  const response = await fetch(`${API_BASE}/admin/auth/verify`, {
    method: 'GET',
    headers: authHeaders()
  });

  const parsed = await parseResponse(response);
  if (!parsed.ok && parsed.status === 401) {
    setToken(null);
  }
  return parsed;
}

export async function adminListIntersections() {
  const response = await fetch(`${API_BASE}/admin/intersections`, {
    method: 'GET',
    headers: authHeaders()
  });
  return parseResponse(response);
}

export async function adminCreateIntersection(payload) {
  const response = await fetch(`${API_BASE}/admin/intersection/create`, {
    method: 'POST',
    headers: authHeaders(),
    body: JSON.stringify(payload)
  });
  return parseResponse(response);
}

export async function adminUpdateIntersection(intersectionId, payload) {
  const response = await fetch(`${API_BASE}/admin/intersection/${intersectionId}`, {
    method: 'PUT',
    headers: authHeaders(),
    body: JSON.stringify(payload)
  });
  return parseResponse(response);
}

export async function adminGetIntersection(intersectionId) {
  const response = await fetch(`${API_BASE}/admin/intersection/${intersectionId}`, {
    method: 'GET',
    headers: authHeaders()
  });
  return parseResponse(response);
}

export async function adminManualControl(intersectionId, phaseId, reason = 'manual_admin_control') {
  const response = await fetch(`${API_BASE}/admin/manual-control`, {
    method: 'POST',
    headers: authHeaders(),
    body: JSON.stringify({ intersection_id: Number(intersectionId), phase_id: Number(phaseId), reason })
  });
  return parseResponse(response);
}

export async function adminNeighbors(intersectionId) {
  const response = await fetch(`${API_BASE}/admin/intersection/${intersectionId}/neighbors`, {
    method: 'GET',
    headers: authHeaders()
  });
  return parseResponse(response);
}

export async function adminListLanes(intersectionId) {
  const response = await fetch(`${API_BASE}/admin/intersection/${intersectionId}/lanes`, {
    method: 'GET',
    headers: authHeaders()
  });
  return parseResponse(response);
}

export async function adminCreateLane(intersectionId, payload) {
  const response = await fetch(`${API_BASE}/admin/intersection/${intersectionId}/lanes`, {
    method: 'POST',
    headers: authHeaders(),
    body: JSON.stringify(payload)
  });
  return parseResponse(response);
}

export async function adminUpdateLane(intersectionId, laneId, payload) {
  const response = await fetch(`${API_BASE}/admin/intersection/${intersectionId}/lanes/${laneId}`, {
    method: 'PUT',
    headers: authHeaders(),
    body: JSON.stringify(payload)
  });
  return parseResponse(response);
}

export async function adminDeleteLane(intersectionId, laneId) {
  const response = await fetch(`${API_BASE}/admin/intersection/${intersectionId}/lanes/${laneId}`, {
    method: 'DELETE',
    headers: authHeaders()
  });
  return parseResponse(response);
}

export async function adminSendEmergency(intersectionId, laneId, vehicleId = 'AMB001') {
  const response = await fetch(`${API_BASE}/intersection/${intersectionId}/simulate-emergency`, {
    method: 'POST',
    headers: authHeaders(),
    body: JSON.stringify({ lane_id: Number(laneId), vehicle_id: vehicleId })
  });
  return parseResponse(response);
}

export async function adminClearEmergency(intersectionId) {
  const response = await fetch(`${API_BASE}/intersection/${intersectionId}/clear-emergency`, {
    method: 'POST',
    headers: authHeaders()
  });
  return parseResponse(response);
}
export async function adminGetLaneConflicts(intersectionId) {
  const response = await fetch(`${API_BASE}/admin/intersection/${intersectionId}/conflicts`, {
    method: 'GET',
    headers: authHeaders()
  });
  return parseResponse(response);
}

export async function adminCreateLaneConflict(intersectionId, laneId1, laneId2, conflictType = 'crossing') {
  const response = await fetch(`${API_BASE}/admin/intersection/${intersectionId}/conflicts`, {
    method: 'POST',
    headers: authHeaders(),
    body: JSON.stringify({
      lane_id_1: Number(laneId1),
      lane_id_2: Number(laneId2),
      conflict_type: conflictType
    })
  });
  return parseResponse(response);
}

export async function adminDeleteLaneConflict(intersectionId, conflictId) {
  const response = await fetch(`${API_BASE}/admin/intersection/${intersectionId}/conflicts/${conflictId}`, {
    method: 'DELETE',
    headers: authHeaders()
  });
  return parseResponse(response);
}

export async function adminGetIntersectionPhaseOptions(intersectionId) {
  const response = await fetch(`${API_BASE}/admin/intersection/${intersectionId}/phase-options`, {
    method: 'GET',
    headers: authHeaders()
  });
  return parseResponse(response);
}

export async function getIntersectionConflicts(intersectionId) {
  const response = await fetch(`${API_BASE}/intersection/${intersectionId}/conflicts`, {
    method: 'GET',
    headers: { 'Content-Type': 'application/json' }
  });
  return parseResponse(response);
}

export async function adminListUsers() {
  const response = await fetch(`${API_BASE}/admin/users`, {
    method: 'GET',
    headers: authHeaders()
  });
  return parseResponse(response);
}

export async function adminCreateUser(payload) {
  const response = await fetch(`${API_BASE}/admin/users`, {
    method: 'POST',
    headers: authHeaders(),
    body: JSON.stringify(payload)
  });
  return parseResponse(response);
}

export async function adminDeleteUser(userId) {
  const response = await fetch(`${API_BASE}/admin/users/${userId}`, {
    method: 'DELETE',
    headers: authHeaders()
  });
  return parseResponse(response);
}

export async function adminChangeUserPassword(userId, payload) {
  const response = await fetch(`${API_BASE}/admin/users/${userId}/password`, {
    method: 'PUT',
    headers: authHeaders(),
    body: JSON.stringify(payload)
  });
  return parseResponse(response);
}