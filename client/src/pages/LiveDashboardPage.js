import { TrafficChartPanel } from '../components/TrafficChartPanel';

function formatTime(timestampSec) {
  if (!timestampSec) return '—';
  return new Date(timestampSec * 1000).toLocaleString('he-IL');
}

function severityClass(eventName) {
  if (eventName === 'action_updated') return 'live-event--action';
  if (eventName === 'state_updated') return 'live-event--state';
  return 'live-event--info';
}

function formatEventTitle(eventName) {
  if (eventName === 'action_updated') return 'עודכנה פעולה';
  if (eventName === 'state_updated') return 'התקבל state חדש';
  if (eventName === 'welcome') return 'חיבור פעיל';
  return eventName || 'event';
}

export function LiveDashboardPage({ metrics, metricsHistory, events, selectedIntersection, status }) {
  const intersections = metrics?.intersections || [];

  return (
    <>
      <div className="top-row live-top-row">
        <div className="card compact">
          <h2>מצב רשת חי</h2>
          <p>{selectedIntersection ? selectedIntersection.name : 'לא נבחרה צומת'}</p>
          <p className="muted page-description">מסך זה מציג KPI רשת, עדכוני WebSocket, ורעננות מצב בזמן אמת.</p>
        </div>

        <div className="card compact">
          <h2>רעננות הנתונים</h2>
          <p>{metrics ? formatTime(metrics.timestamp) : 'טוען...'}</p>
          <p className="muted">עדכון אחרון מהשרת המרכזי</p>
        </div>
      </div>

      <div className="stats-grid live-kpi-grid">
        <div className="stat-box live-kpi">
          <span>מספר צמתים פעילים</span>
          <strong>{metrics?.intersection_count ?? 0}</strong>
        </div>
        <div className="stat-box live-kpi">
          <span>תור רשת כולל</span>
          <strong>{metrics?.total_network_queue ?? 0}</strong>
        </div>
        <div className="stat-box live-kpi">
          <span>המתנה ממוצעת ברשת</span>
          <strong>{Math.round(metrics?.avg_network_waiting_sec ?? 0)} sec</strong>
        </div>
        <div className="stat-box live-kpi">
          <span>מצב צומת נבחרת</span>
          <strong>{status ? status.currentPhase : '—'}</strong>
        </div>
      </div>

      <div className="live-grid">
        <div className="live-chart-card">
          <TrafficChartPanel status={status} />
        </div>

        <div className="card live-card">
          <h2>צמתים ברשת</h2>
          <div className="live-network-list">
            {intersections.length === 0 ? (
              <div className="muted">אין כרגע נתונים מהשרת.</div>
            ) : (
              intersections.map((item) => (
                <div key={item.intersection_id} className="live-network-item">
                  <div>
                    <strong>צומת #{item.intersection_id}</strong>
                    <div className="muted">נתיבים: {item.num_lanes} · עדכון: {formatTime(metrics?.timestamp)}</div>
                  </div>
                  <div className="live-network-item-metrics">
                    <span>Queue: {item.total_queue}</span>
                    <span>Wait: {Math.round(item.avg_waiting_sec)}s</span>
                    <span className={item.emergency_active ? 'tag tag-danger' : 'tag tag-ok'}>
                      {item.emergency_active ? 'חירום' : 'תקין'}
                    </span>
                  </div>
                </div>
              ))
            )}
          </div>
        </div>

        <div className="card live-card">
          <h2>אירועים אחרונים</h2>
          <div className="live-event-feed">
            {events.length === 0 ? (
              <div className="muted">אין עדיין אירועים חיים.</div>
            ) : (
              events.map((entry, index) => (
                <div key={`${entry.timestamp}-${index}`} className={`live-event ${severityClass(entry.event)}`}>
                  <div className="live-event-header">
                    <strong>{formatEventTitle(entry.event)}</strong>
                    <span className="muted">{new Date(entry.timestamp * 1000).toLocaleTimeString('he-IL')}</span>
                  </div>
                  <pre className="live-event-payload">{JSON.stringify(entry.payload, null, 2)}</pre>
                </div>
              ))
            )}
          </div>
        </div>
      </div>
    </>
  );
}