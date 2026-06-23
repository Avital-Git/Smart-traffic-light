import { useEffect, useMemo, useRef, useState } from 'react';
import { Navigate, Route, Routes } from 'react-router-dom';
import { Sidebar } from './components/Sidebar';
import { TopBar } from './components/TopBar';
import { ControlPage } from './pages/ControlPage';
import { LiveDashboardPage } from './pages/LiveDashboardPage';
import { NetworkViewPage } from './pages/NetworkViewPage';
import { OverviewPage } from './pages/OverviewPage';
import { AdminPage } from './pages/AdminPage';
import {
  getIntersectionLayout,
  getIntersectionStatus,
  getIntersections,
  getSystemConfig,
  getNetworkMetrics,
  sendManualControl,
  subscribeGlobalUpdates,
  subscribeIntersectionUpdates
} from './services/api';
import { adminClearEmergency, adminSendEmergency, getAdminToken, verifyAdminToken } from './services/adminApi';
import './App.css';

const DEFAULT_EMERGENCY_UI_HOLD_MS = 30000;

function App() {
  const [intersections, setIntersections] = useState([]);
  const [selectedId, setSelectedId] = useState(null);
  const [status, setStatus] = useState(null);
  const [metrics, setMetrics] = useState(null);
  const [metricsHistory, setMetricsHistory] = useState([]);
  const [events, setEvents] = useState([]);
  const [message, setMessage] = useState('');
  const [error, setError] = useState('');
  const [manualEmergencyEnabled, setManualEmergencyEnabled] = useState(true);
  const [emergencySubmitting, setEmergencySubmitting] = useState(false);
  const [emergencyStatusText, setEmergencyStatusText] = useState('');
  const [emergencyUiHoldMs, setEmergencyUiHoldMs] = useState(DEFAULT_EMERGENCY_UI_HOLD_MS);
  const [isAdmin, setIsAdmin] = useState(false);
  const [adminAuthChecked, setAdminAuthChecked] = useState(false);
  const statusFailureCountRef = useRef(0);

  const applyEmergencyUiHold = (nextStatus, currentStatus = null, source = null) => {
    if (!nextStatus) return nextStatus;

    const nowMs = Date.now();
    const signal = nextStatus.emergency_signal || null;
    const signalActive = Boolean(signal?.active);
    const emergencyCleared = source === 'emergency_cleared';
    const emergencyPreempt = source === 'emergency_preempt';
    const prevUiUntil = Number(currentStatus?.emergencyUiUntil) || 0;

    const nextUiUntil = emergencyCleared
      ? 0
      : ((signalActive || emergencyPreempt) ? (nowMs + emergencyUiHoldMs) : prevUiUntil);

    const emergencyActive = !emergencyCleared && (signalActive || emergencyPreempt || nextUiUntil > nowMs);

    const parsedLaneFromSignal = Number(signal?.lane_id);
    const parsedLaneFromStatus = Number(nextStatus.emergencyLaneId ?? nextStatus.emergency_lane_id);
    const parsedLaneFromCurrent = Number(currentStatus?.emergencyLaneId ?? currentStatus?.emergency_lane_id);

    const emergencyLaneId = emergencyActive
      ? (Number.isFinite(parsedLaneFromSignal)
          ? parsedLaneFromSignal
          : (Number.isFinite(parsedLaneFromStatus)
              ? parsedLaneFromStatus
              : (Number.isFinite(parsedLaneFromCurrent) ? parsedLaneFromCurrent : null)))
      : null;

    const emergencyVehicleId = emergencyActive
      ? (signal?.vehicle_id ?? nextStatus.emergencyVehicleId ?? currentStatus?.emergencyVehicleId ?? null)
      : null;

    return {
      ...nextStatus,
      emergencyUiUntil: emergencyActive ? nextUiUntil : null,
      emergencyActive,
      emergencyLaneId,
      emergencyVehicleId,
      emergency_signal: emergencyActive
        ? {
            active: true,
            lane_id: emergencyLaneId,
            vehicle_id: emergencyVehicleId,
          }
        : null,
      alerts: emergencyActive
        ? [{
            id: `emergency-${nextStatus.intersectionId ?? currentStatus?.intersectionId ?? 'selected'}`,
            severity: 'high',
            message: `חירום פעיל בנתיב ${emergencyLaneId ?? '-'}`,
            timestamp: new Date().toLocaleTimeString('he-IL')
          }]
        : [{
            id: `normal-${nextStatus.intersectionId ?? currentStatus?.intersectionId ?? 'selected'}`,
            severity: 'low',
            message: 'אין התרעות חירום פעילות',
            timestamp: new Date().toLocaleTimeString('he-IL')
          }]
    };
  };

  useEffect(() => {
    getIntersections().then((items) => {
      setIntersections(items);
      if (items.length > 0) {
        setSelectedId(items[0].id);
      }
    });

    getSystemConfig().then((config) => {
      const enabled = config?.hardware?.manual_emergency_enabled;
      const latchSec = Number(config?.hardware?.emergency_latch_sec);
      setManualEmergencyEnabled(enabled !== false);
      if (Number.isFinite(latchSec) && latchSec > 0) {
        setEmergencyUiHoldMs(Math.round(latchSec * 1000));
      }
    });

    const syncAdmin = async () => {
      const token = getAdminToken();
      if (!token) {
        setIsAdmin(false);
        setAdminAuthChecked(true);
        return;
      }

      setAdminAuthChecked(false);
      const verify = await verifyAdminToken();
      setIsAdmin(Boolean(verify?.ok));
      setAdminAuthChecked(true);
    };

    syncAdmin();
    const onStorage = () => syncAdmin();
    const onAdminTokenChanged = () => syncAdmin();
    const onFocus = () => syncAdmin();
    window.addEventListener('storage', onStorage);
    window.addEventListener('smart-traffic-admin-token-changed', onAdminTokenChanged);
    window.addEventListener('focus', onFocus);

    return () => {
      window.removeEventListener('storage', onStorage);
      window.removeEventListener('smart-traffic-admin-token-changed', onAdminTokenChanged);
      window.removeEventListener('focus', onFocus);
    };
  }, []);

  useEffect(() => {
    let isMounted = true;

    const loadMetrics = async () => {
      try {
        const nextMetrics = await getNetworkMetrics();
        if (!nextMetrics) {
          return;
        }
        if (isMounted) {
          setMetrics(nextMetrics);
          setMetricsHistory((current) => {
            const nextPoint = {
              timestamp: nextMetrics.timestamp,
              timeLabel: new Date((nextMetrics.timestamp || Date.now() / 1000) * 1000).toLocaleTimeString('he-IL', {
                hour: '2-digit',
                minute: '2-digit',
                second: '2-digit'
              }),
              totalQueue: nextMetrics.total_network_queue || 0,
              avgWait: Math.round(nextMetrics.avg_network_waiting_sec || 0),
              activeIntersections: nextMetrics.intersection_count || 0
            };

            return [...current, nextPoint].slice(-30);
          });
        }
      } catch {
        if (isMounted) {
          setMetrics(null);
        }
      }
    };

    loadMetrics();
    const interval = window.setInterval(loadMetrics, 10000);

    return () => {
      isMounted = false;
      window.clearInterval(interval);
    };
  }, []);

  useEffect(() => {
    const unsubscribe = subscribeGlobalUpdates((event) => {
      setEvents((current) => [{ ...event }, ...current].slice(0, 20));

      const intersectionId = Number(event?.intersection_id);
      if (!Number.isFinite(intersectionId)) {
        return;
      }

      if (event?.event === 'action_updated' && Number(selectedId) === intersectionId) {
        const action = event?.payload?.action || {};
        const source = event?.payload?.source || '';
        const signal = event?.payload?.signal || {};

        setStatus((current) => {
          if (!current) return current;

          const baseStatus = {
            ...current,
            currentPhase: action?.action || current.currentPhase,
            actionSource: source || current.actionSource,
            emergency_signal: signal,
            emergencyLaneId: Number.isFinite(Number(signal?.lane_id)) ? Number(signal.lane_id) : current.emergencyLaneId,
            emergencyVehicleId: signal?.vehicle_id ?? current.emergencyVehicleId,
            intersectionId
          };

          return applyEmergencyUiHold(baseStatus, current, source);
        });
        return;
      }

      if (event?.event === 'state_updated') {
        const state = event?.payload?.state;

        if (!Number.isFinite(intersectionId) || !state) {
          return;
        }

        // Instant update for selected intersection (no refresh needed).
        if (Number(selectedId) === intersectionId) {
          getIntersectionLayout(intersectionId).then((layout) => {
            const currentPhase = event?.payload?.action?.action || status?.currentPhase || 'Hold';
            const lanes = state?.lanes || [];
            const laneDirections = Array.isArray(layout?.lane_directions)
              ? layout.lane_directions
              : [];

            const directionLabel = (laneId) => {
              const d = laneDirections[laneId];
              if (d === 'N') return 'צפון';
              if (d === 'S') return 'דרום';
              if (d === 'E') return 'מזרח';
              if (d === 'W') return 'מערב';
              return `נתיב ${laneId}`;
            };

            const totalQueue = lanes.reduce((sum, lane) => sum + (lane.vehicle_count || 0), 0);
            const avgWaitSec = lanes.length > 0
              ? Math.round(lanes.reduce((sum, lane) => sum + (lane.waiting_time_sec || 0), 0) / lanes.length)
              : 0;

            setStatus((current) => {
              const baseStatus = {
                intersectionId,
                currentPhase,
                congestionLevel: totalQueue >= 55 ? 'HIGH' : totalQueue >= 30 ? 'MEDIUM' : 'LOW',
                totalQueue,
                avgWaitSec,
                manualOverrideEnabled: false,
                emergencyActive: Boolean(state?.emergency_signal?.active),
                emergencyLaneId: state?.emergency_signal?.lane_id ?? null,
                emergencyVehicleId: state?.emergency_signal?.vehicle_id ?? null,
                emergency_signal: state?.emergency_signal ?? null,
                updatedAt: new Date((state.timestamp || Date.now() / 1000) * 1000).toLocaleString('he-IL'),
                signals: lanes.map((lane) => ({
                  direction: directionLabel(lane.lane_id),
                  color: lane.signal_color || 'RED',
                  queue: lane.vehicle_count,
                  waitingSec: lane.waiting_time_sec
                })),
                chart: lanes.map((lane, index) => ({
                  time: `נתיב ${lane.lane_id}`,
                  queue: lane.vehicle_count,
                  avgWaitSec: lane.waiting_time_sec,
                  index
                })),
                lanes,
                laneDirections,
                actionSource: event?.payload?.source || null,
                neighbors: Array.isArray(layout?.neighbors) ? layout.neighbors : []
              };

              return applyEmergencyUiHold(baseStatus, current, event?.payload?.source || null);
            });
          });
          return;
        }

        // Instant metrics/network card refresh (no polling wait).
        setMetrics((current) => {
          if (!current || !Array.isArray(current.intersections)) {
            return current;
          }

          const lanes = state?.lanes || [];
          const total_queue = lanes.reduce((sum, lane) => sum + Math.max(0, lane.vehicle_count || 0), 0);
          const avg_waiting_sec = lanes.length
            ? (lanes.reduce((sum, lane) => sum + Math.max(0, lane.waiting_time_sec || 0), 0) / lanes.length)
            : 0;

          const nextIntersections = current.intersections.map((item) => {
            if (Number(item.intersection_id) !== intersectionId) {
              return item;
            }
            return {
              ...item,
              num_lanes: state?.num_lanes ?? item.num_lanes,
              total_queue,
              avg_waiting_sec,
              emergency_active: Boolean(state?.emergency_signal?.active),
              state_age_sec: 0,
              last_action: event?.payload?.action || item.last_action
            };
          });

          const totalNetworkQueue = nextIntersections.reduce((sum, item) => sum + (item.total_queue || 0), 0);
          const avgNetworkWait = nextIntersections.length
            ? (nextIntersections.reduce((sum, item) => sum + (item.avg_waiting_sec || 0), 0) / nextIntersections.length)
            : 0;

          return {
            ...current,
            timestamp: event?.timestamp || current.timestamp,
            intersections: nextIntersections,
            total_network_queue: totalNetworkQueue,
            avg_network_waiting_sec: avgNetworkWait
          };
        });
      }
    });

    return unsubscribe;
  }, [selectedId, status?.currentPhase]);

  useEffect(() => {
    if (!selectedId) return undefined;

    statusFailureCountRef.current = 0;
    setError('');

    const load = async () => {
      try {
        const data = await getIntersectionStatus(selectedId);
        if (!data) {
          statusFailureCountRef.current += 1;
          if (statusFailureCountRef.current >= 3) {
            setError('אין כרגע נתוני זמן-אמת לצומת שנבחרה.');
          }
          return;
        }

        statusFailureCountRef.current = 0;
        setStatus((current) => applyEmergencyUiHold(data, current, data?.actionSource || null));
        setError('');
      } catch {
        statusFailureCountRef.current += 1;
        if (statusFailureCountRef.current >= 3) {
          setError('שגיאת תקשורת זמנית עם השרת.');
        }
      }
    };

    load();

    const unsubscribe = subscribeIntersectionUpdates(selectedId, (nextStatus) => {
      setStatus((current) => applyEmergencyUiHold(nextStatus, current, nextStatus?.actionSource || null));
      setError('');
    });

    const interval = window.setInterval(load, 10000);
    return () => {
      unsubscribe();
      window.clearInterval(interval);
    };
  }, [selectedId]);

  const selectedIntersection = useMemo(
    () => intersections.find((item) => item.id === selectedId) || null,
    [intersections, selectedId]
  );

  async function handleManualControl(action, enabled) {
    if (!isAdmin) {
      setMessage('גישה לשליטה ידנית מותרת למנהל בלבד.');
      return;
    }
    if (!selectedId) return;
    const result = await sendManualControl(selectedId, action, enabled);
    setMessage(result.ok ? 'הפקודה נשלחה בהצלחה' : `שליחת הפקודה נכשלה: ${result.detail || 'שגיאה לא ידועה'}`);
  }

  async function handleTriggerEmergency(laneId, vehicleId) {
    if (!isAdmin) {
      setMessage('גישה לשליטה ידנית מותרת למנהל בלבד.');
      return;
    }
    if (!selectedId) return;
    if (!Number.isFinite(Number(laneId))) {
      setMessage('בחירת נתיב חירום אינה תקינה');
      return;
    }

    setEmergencySubmitting(true);
    setEmergencyStatusText(`שולח קריאת חירום לנתיב ${laneId}...`);

    const result = await adminSendEmergency(selectedId, Number(laneId), vehicleId || 'AMB001');
    setEmergencySubmitting(false);

    if (result.ok) {
      setEmergencyStatusText(`קריאת חירום נשלחה בהצלחה לנתיב ${laneId}`);
      setMessage('אות חירום נשלח בהצלחה');
    } else {
      setEmergencyStatusText('');
      setMessage(`שליחת חירום נכשלה: ${result.detail || 'שגיאה לא ידועה'}`);
    }
  }

  async function handleClearEmergency() {
    if (!isAdmin) {
      setMessage('גישה לשליטה ידנית מותרת למנהל בלבד.');
      return;
    }
    if (!selectedId) return;
    setEmergencySubmitting(true);
    const result = await adminClearEmergency(selectedId);
    setEmergencySubmitting(false);

    if (result.ok) {
      setEmergencyStatusText('מצב חירום נוקה');
      setMessage('אות חירום נוקה');
    } else {
      setMessage(`ניקוי חירום נכשל: ${result.detail || 'שגיאה לא ידועה'}`);
    }
  }

  const connectionStatus = status ? 'online' : 'offline';
  const canAccessManualControl = adminAuthChecked && isAdmin;

  return (
    <div className="app-shell">
      <Sidebar
        selectedIntersection={selectedIntersection}
        connectionStatus={connectionStatus}
        isAdmin={canAccessManualControl}
      />

      <div className="main-wrapper">
        <TopBar
          selectedIntersection={selectedIntersection}
          connectionStatus={connectionStatus}
        />

        {error && <div className="error-banner">{error}</div>}

        <main className="page-content">
          <Routes>
            <Route path="/" element={<Navigate to="/overview" replace />} />
            <Route
              path="/live"
              element={
                <LiveDashboardPage
                  metrics={metrics}
                  metricsHistory={metricsHistory}
                  events={events}
                  selectedIntersection={selectedIntersection}
                  status={status}
                />
              }
            />
            <Route
              path="/overview"
              element={
                <OverviewPage
                  intersections={intersections}
                  selectedId={selectedId}
                  onSelect={setSelectedId}
                  selectedIntersection={selectedIntersection}
                  status={status}
                />
              }
            />
            <Route
              path="/network"
              element={
                <NetworkViewPage
                  intersections={intersections}
                  selectedIntersection={selectedIntersection}
                  status={status}
                />
              }
            />
            <Route
              path="/control/*"
              element={
                !adminAuthChecked ? (
                  <div className="card compact">
                    <h2>שליטה ידנית</h2>
                    <p>מאמת הרשאות...</p>
                  </div>
                ) : canAccessManualControl ? (
                  <ControlPage
                    intersections={intersections}
                    selectedId={selectedId}
                    onSelect={setSelectedId}
                    selectedIntersection={selectedIntersection}
                    status={status}
                    message={message}
                    isAdmin={canAccessManualControl}
                    manualEmergencyEnabled={manualEmergencyEnabled}
                    onSendManualControl={handleManualControl}
                    onTriggerEmergency={handleTriggerEmergency}
                    onClearEmergency={handleClearEmergency}
                    emergencySubmitting={emergencySubmitting}
                    emergencyStatusText={emergencyStatusText}
                  />
                ) : (
                  <Navigate to="/live" replace />
                )
              }
            />
            <Route
              path="/admin"
              element={<AdminPage />}
            />
          </Routes>
        </main>
      </div>
    </div>
  );
}

export default App;
