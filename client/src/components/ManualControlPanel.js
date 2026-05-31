import { useState } from 'react';

export function ManualControlPanel({
  onSend,
  onTriggerEmergency,
  onClearEmergency,
  manualEmergencyEnabled
}) {
  const [enabled, setEnabled] = useState(false);
  const [action, setAction] = useState('Phase0');
  const [emergencyLaneId, setEmergencyLaneId] = useState(0);
  const [vehicleId, setVehicleId] = useState('AMB001');

  return (
    <div className="card">
      <h2>שליטה ידנית</h2>
      <label className="checkbox-row">
        <input type="checkbox" checked={enabled} onChange={(e) => setEnabled(e.target.checked)} />
        הפעלת שליטה ידנית
      </label>

      <select className="select" value={action} onChange={(e) => setAction(e.target.value)}>
        <option value="Phase0">Phase0</option>
        <option value="Phase1">Phase1</option>
        <option value="Hold">Hold</option>
      </select>

      <button className="button primary" onClick={() => onSend(action, enabled)}>
        שלח פקודה
      </button>

      {manualEmergencyEnabled ? (
        <>
          <hr style={{ margin: '16px 0', opacity: 0.2 }} />
          <h3 style={{ marginTop: 0 }}>שליחת חירום (סימולציה)</h3>

          <select
            className="select"
            value={emergencyLaneId}
            onChange={(e) => setEmergencyLaneId(Number(e.target.value))}
          >
            <option value={0}>נתיב 0 (N)</option>
            <option value={1}>נתיב 1 (S)</option>
            <option value={2}>נתיב 2 (E)</option>
            <option value={3}>נתיב 3 (W)</option>
          </select>

          <select
            className="select"
            value={vehicleId}
            onChange={(e) => setVehicleId(e.target.value)}
          >
            <option value="AMB001">AMB001 (אמבולנס)</option>
            <option value="POL001">POL001 (משטרה)</option>
            <option value="FIRE001">FIRE001 (כבאות)</option>
            <option value="MED001">MED001 (רפואה)</option>
          </select>

          <div style={{ display: 'flex', gap: 8 }}>
            <button
              className="button danger"
              onClick={() => onTriggerEmergency(emergencyLaneId, vehicleId)}
            >
              שלח חירום
            </button>
            <button className="button" onClick={() => onClearEmergency()}>
              נקה חירום
            </button>
          </div>
        </>
      ) : (
        <>
          <hr style={{ margin: '16px 0', opacity: 0.2 }} />
          <p style={{ margin: 0, opacity: 0.85 }}>
            מצב חומרה אמיתית פעיל — כפתור חירום בדשבורד מוסתר.
          </p>
        </>
      )}
    </div>
  );
}
