import { EmergencyAlertsPanel } from '../components/EmergencyAlertsPanel';
import { IntersectionSelector } from '../components/IntersectionSelector';
import { SignalStatusPanel } from '../components/SignalStatusPanel';
import { TrafficChartPanel } from '../components/TrafficChartPanel';
import { TrafficStatusCard } from '../components/TrafficStatusCard';

export function OverviewPage({ intersections, selectedId, onSelect, selectedIntersection, status }) {
  return (
    <>
      <div className="top-row">
        <IntersectionSelector
          intersections={intersections}
          selectedId={selectedId}
          onChange={onSelect}
        />

        <div className="card compact">
          <h2>צומת נבחרת</h2>
          <p>{selectedIntersection ? selectedIntersection.name : 'לא נבחרה צומת'}</p>
          <p className="muted page-description">בדף זה אפשר לעקוב אחרי מצב התנועה והעומסים בזמן אמת.</p>
        </div>
      </div>

      <TrafficStatusCard status={status} />

      <div className="dashboard-grid">
        <SignalStatusPanel status={status} />
        <EmergencyAlertsPanel status={status} />
        <TrafficChartPanel status={status} />
      </div>
    </>
  );
}
