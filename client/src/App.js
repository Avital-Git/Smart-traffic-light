import { useEffect, useMemo, useState } from 'react';
import { Navigate, Route, Routes } from 'react-router-dom';
import { Sidebar } from './components/Sidebar';
import { TopBar } from './components/TopBar';
import { ControlPage } from './pages/ControlPage';
import { LiveDashboardPage } from './pages/LiveDashboardPage';
import { OverviewPage } from './pages/OverviewPage';
import {
  getIntersectionStatus,
  getIntersections,
  getNetworkMetrics,
  sendManualControl,
  subscribeGlobalUpdates,
  subscribeIntersectionUpdates
} from './services/api';
import './App.css';

function App() {
  const [intersections, setIntersections] = useState([]);
  const [selectedId, setSelectedId] = useState(null);
  const [status, setStatus] = useState(null);
  const [metrics, setMetrics] = useState(null);
  const [metricsHistory, setMetricsHistory] = useState([]);
  const [events, setEvents] = useState([]);
  const [message, setMessage] = useState('');
  const [error, setError] = useState('');

  useEffect(() => {
    getIntersections().then((items) => {
      setIntersections(items);
      if (items.length > 0) {
        setSelectedId(items[0].id);
      }
    });
  }, []);

  useEffect(() => {
    let isMounted = true;

    const loadMetrics = async () => {
      try {
        const nextMetrics = await getNetworkMetrics();
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
    });

    return unsubscribe;
  }, []);

  useEffect(() => {
    if (!selectedId) return undefined;

    const load = async () => {
      try {
        const data = await getIntersectionStatus(selectedId);
        setStatus(data);
        setError('');
      } catch {
        setError('לא ניתן לטעון את מצב הצומת כרגע.');
      }
    };

    load();

    const unsubscribe = subscribeIntersectionUpdates(selectedId, (nextStatus) => {
      setStatus(nextStatus);
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
    if (!selectedId) return;
    const result = await sendManualControl(selectedId, action, enabled);
    setMessage(result.ok ? 'הפקודה נשלחה בהצלחה' : 'שליחת הפקודה נכשלה');
  }

  const connectionStatus = status ? 'online' : 'offline';

  return (
    <div className="app-shell">
      <Sidebar
        selectedIntersection={selectedIntersection}
        connectionStatus={connectionStatus}
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
              path="/control"
              element={
                <ControlPage
                  intersections={intersections}
                  selectedId={selectedId}
                  onSelect={setSelectedId}
                  selectedIntersection={selectedIntersection}
                  status={status}
                  message={message}
                  onSendManualControl={handleManualControl}
                />
              }
            />
          </Routes>
        </main>
      </div>
    </div>
  );
}

export default App;
