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
  isAdmin,
  manualEmergencyEnabled,
  onSendManualControl,
  onTriggerEmergency,
  onClearEmergency
}) {
  const subtitle = selectedIntersection
    ? `${selectedIntersection.name} - ${isAdmin ? 'שליטה ידנית זמינה' : 'צפייה בלבד'}`
    : 'לא נבחרה צומת';

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
          <p>{subtitle}</p>
          {!isAdmin && (
            <div className="muted" style={{ marginTop: 6 }}>
              משתמש רגיל: מוצג מצב צומת בלבד ללא כלי שליטה.
            </div>
          )}
          {message && <div className="message">{message}</div>}
        </div>
      </div>

      <TrafficStatusCard status={status} />

      <div className={isAdmin ? 'dashboard-grid control-grid' : 'dashboard-grid'}>
        {isAdmin && (
          <ManualControlPanel
            onSend={onSendManualControl}
            onTriggerEmergency={onTriggerEmergency}
            onClearEmergency={onClearEmergency}
            manualEmergencyEnabled={manualEmergencyEnabled}
          />
        )}
        <SignalStatusPanel status={status} />
        <EmergencyAlertsPanel status={status} />
      </div>
    </>
  );
}
