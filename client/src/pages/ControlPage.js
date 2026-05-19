import { EmergencyAlertsPanel } from '../components/EmergencyAlertsPanel';
import { IntersectionSelector } from '../components/IntersectionSelector';
import { ManualControlPanel } from '../components/ManualControlPanel';
import { SignalStatusPanel } from '../components/SignalStatusPanel';
import { TrafficStatusCard } from '../components/TrafficStatusCard';

export function ControlPage({
  intersections,
  selectedId,
  onSelect,
  selectedIntersection,
  status,
  message,
  onSendManualControl
}) {
  return (
    <>
      <div className="top-row">
        <IntersectionSelector
          intersections={intersections}
          selectedId={selectedId}
          onChange={onSelect}
        />

        <div className="card compact">
          <h2>שליטת רמזורים</h2>
          <p>{selectedIntersection ? `${selectedIntersection.name} - שליטה ידנית ובקרה` : 'לא נבחרה צומת'}</p>
          {message && <div className="message">{message}</div>}
        </div>
      </div>

      <TrafficStatusCard status={status} />

      <div className="dashboard-grid control-grid">
        <ManualControlPanel onSend={onSendManualControl} />
        <SignalStatusPanel status={status} />
        <EmergencyAlertsPanel status={status} />
      </div>
    </>
  );
}
