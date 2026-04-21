/**
 * App.js
 * ------
 * דשבורד ראשי — מערכת ניהול תנועה חכמה
 * אביטל חדד | מכללת בנות בת שבע
 *
 * מציג בזמן אמת את מצב כל הצמתים מהשרת.
 */

import { useState, useEffect, useCallback } from 'react';
import './App.css';

const API_BASE = 'http://127.0.0.1:8000';

function App() {
  const [health, setHealth] = useState(null);
  const [intersections, setIntersections] = useState({});
  const [error, setError] = useState(null);

  // בדיקת תקינות שרת
  const checkHealth = useCallback(async () => {
    try {
      const res = await fetch(`${API_BASE}/health`);
      const data = await res.json();
      setHealth(data);
      setError(null);
    } catch (e) {
      setError('לא ניתן להתחבר לשרת. ודאי שהשרת רץ.');
      setHealth(null);
    }
  }, []);

  // קריאת מצב צמתים
  const fetchIntersections = useCallback(async () => {
    const ids = [1, 2, 3, 4];
    const results = {};
    for (const id of ids) {
      try {
        const res = await fetch(`${API_BASE}/intersection/${id}`);
        if (res.ok) {
          results[id] = await res.json();
        }
      } catch {
        // צומת לא נמצאה — בסדר
      }
    }
    setIntersections(results);
  }, []);

  useEffect(() => {
    checkHealth();
    const interval = setInterval(() => {
      checkHealth();
      fetchIntersections();
    }, 2000);
    return () => clearInterval(interval);
  }, [checkHealth, fetchIntersections]);

  return (
    <div className="app">
      <header className="header">
        <h1>🚦 Smart Traffic Dashboard</h1>
        <p>מערכת ניהול תנועה חכמה — צפייה בזמן אמת</p>
        <div className={`status-badge ${health ? 'online' : 'offline'}`}>
          {health ? '🟢 שרת פעיל' : '🔴 שרת לא זמין'}
        </div>
      </header>

      {error && <div className="error-banner">{error}</div>}

      <main className="dashboard">
        {Object.keys(intersections).length === 0 ? (
          <div className="no-data">
            <p>⏳ ממתין לנתונים מהצמתים...</p>
            <p>הפעילי את הסימולציה או את auto_launcher.py</p>
          </div>
        ) : (
          <div className="grid">
            {Object.entries(intersections).map(([id, state]) => (
              <IntersectionCard key={id} state={state} />
            ))}
          </div>
        )}
      </main>

      <footer className="footer">
        <p>אביטל חדד | מכללת בנות בת שבע | פרויקט גמר</p>
      </footer>
    </div>
  );
}


function IntersectionCard({ state }) {
  const totalVehicles = state.lanes
    ? state.lanes.reduce((s, l) => s + l.vehicle_count, 0)
    : 0;
  const maxDensity = state.lanes
    ? Math.max(...state.lanes.map(l => l.density_pct))
    : 0;

  const urgencyClass = maxDensity >= 70 ? 'high' : maxDensity >= 40 ? 'medium' : 'low';

  return (
    <div className={`card ${urgencyClass}`}>
      <div className="card-header">
        <h2>צומת #{state.intersection_id}</h2>
        <span className="badge">{state.num_lanes} נתיבים</span>
      </div>

      <div className="stats">
        <div className="stat">
          <span className="stat-value">{totalVehicles}</span>
          <span className="stat-label">רכבים</span>
        </div>
        <div className="stat">
          <span className="stat-value">{maxDensity.toFixed(0)}%</span>
          <span className="stat-label">צפיפות מקס׳</span>
        </div>
      </div>

      <div className="lanes">
        {state.lanes && state.lanes.map(lane => (
          <div key={lane.lane_id} className="lane-row">
            <span className="lane-label">נתיב {lane.lane_id}</span>
            <div className="lane-bar-container">
              <div
                className="lane-bar"
                style={{ width: `${Math.min(lane.density_pct, 100)}%` }}
              />
            </div>
            <span className="lane-info">
              V={lane.vehicle_count} P={lane.pedestrian_count} W={lane.waiting_time_sec.toFixed(0)}s
            </span>
          </div>
        ))}
      </div>
    </div>
  );
}


export default App;
