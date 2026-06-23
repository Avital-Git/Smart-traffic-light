import { EmergencyAlertsPanel } from '../components/EmergencyAlertsPanel';
import { IntersectionSelector } from '../components/IntersectionSelector';
import { ManualControlPanel } from '../components/ManualControlPanel';
import { SignalStatusPanel } from '../components/SignalStatusPanel';
import { TrafficStatusCard } from '../components/TrafficStatusCard';
import { IntersectionVisual } from '../components/TrafficChartPanel';

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
  onClearEmergency,
  emergencySubmitting,
  emergencyStatusText
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

      <div className="card" style={{ marginBottom: 16 }}>
        <h2>ויזואליזציית צומת בזמן אמת</h2>
        {status ? <IntersectionVisual status={status} /> : <div className="muted">אין נתוני צומת להצגה</div>}
      </div>

      <div className={isAdmin ? 'dashboard-grid control-grid' : 'dashboard-grid'}>
        {isAdmin && (
          <ManualControlPanel
            status={status}
            onSend={onSendManualControl}
            onTriggerEmergency={onTriggerEmergency}
            onClearEmergency={onClearEmergency}
            manualEmergencyEnabled={manualEmergencyEnabled}
            emergencySubmitting={emergencySubmitting}
            emergencyStatusText={emergencyStatusText}
          />
        )}
        <SignalStatusPanel status={status} />
        <EmergencyAlertsPanel status={status} />
      </div>
    </>
  );
}
