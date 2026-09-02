import { useEffect, useMemo, useState } from 'react';

export function ManualControlPanel({
  status,
  onSend,
  onTriggerEmergency,
  onClearEmergency,
  manualEmergencyEnabled,
  emergencySubmitting,
  emergencyStatusText
}) {
  const [enabled, setEnabled] = useState(false);
  const [action, setAction] = useState('Phase0');
  const [emergencyLaneId, setEmergencyLaneId] = useState('');
  const [vehicleId, setVehicleId] = useState('AMB001');

  const emergencyActive = Boolean(
    status?.emergencyActive ||
    status?.emergency_active ||
    status?.emergency_signal?.active ||
    status?.state?.emergency_signal?.active ||
    status?.actionSource === 'emergency_preempt' || status?.actionSource === 'emergency_preempt_gps' // emergency_preempt_gps = חירום שהגיע מ-GPS locate (POST /emergency/locate)
  );

  const emergencyLaneOptions = useMemo(() => {
    const liveLanes = Array.isArray(status?.lanes) ? status.lanes : [];
    return liveLanes
      .map((lane, index) => {
        const laneId = Number(lane?.lane_id ?? lane?.id);
        if (!Number.isFinite(laneId)) return null;
        return {
          value: String(laneId),
          label: `Lane #${laneId} · ${lane?.direction || lane?.lane_direction || '?'} · עומס ${Number(lane?.vehicle_count || 0)}`,
          sortOrder: Number.isFinite(Number(lane?.camera_index)) ? Number(lane.camera_index) : index,
        };
      })
      .filter(Boolean)
      .sort((a, b) => a.sortOrder - b.sortOrder);
  }, [status]);

  useEffect(() => {
    if (!emergencyLaneOptions.length) {
      setEmergencyLaneId('');
      return;
    }

    const exists = emergencyLaneOptions.some((lane) => lane.value === emergencyLaneId);
    if (!exists) {
      setEmergencyLaneId(emergencyLaneOptions[0].value);
    }
  }, [emergencyLaneOptions, emergencyLaneId]);

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
            onChange={(e) => setEmergencyLaneId(e.target.value)}
            disabled={!emergencyLaneOptions.length || emergencySubmitting || emergencyActive}
          >
            {!emergencyLaneOptions.length && <option value="">אין נתיבים זמינים בצומת</option>}
            {emergencyLaneOptions.map((lane) => (
              <option key={lane.value} value={lane.value}>{lane.label}</option>
            ))}
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
              onClick={() => onTriggerEmergency(Number(emergencyLaneId), vehicleId)}
              disabled={!emergencyLaneOptions.length || emergencyLaneId === '' || emergencySubmitting || emergencyActive}
            >
              {emergencySubmitting ? 'שולח חירום...' : 'שלח חירום'}
            </button>
            <button className="button" onClick={() => onClearEmergency()} disabled={emergencySubmitting || !emergencyActive}>
              נקה חירום
            </button>
          </div>

          {emergencyStatusText && (
            <div className="badge badge-ok" style={{ marginTop: 10 }}>
              {emergencyStatusText}
            </div>
          )}

          {emergencyActive && (
            <div className="badge badge-danger" style={{ marginTop: 10 }}>
              מצב חירום פעיל בצומת זו
            </div>
          )}
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
