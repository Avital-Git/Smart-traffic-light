export function EmergencyAlertsPanel({ status }) {
  if (!status) return null;

  return (
    <div className="card">
      <h2>התרעות חירום</h2>
      <div className={status.emergencyActive ? 'badge badge-danger' : 'badge badge-ok'}>
        {status.emergencyActive ? 'חירום פעיל' : 'אין אירועי חירום'}
      </div>

      <div className="alert-list">
        {status.alerts.map((alert) => (
          <div key={alert.id} className={`alert-item severity-${alert.severity}`}>
            <strong>{alert.message}</strong>
            <span>{alert.timestamp}</span>
          </div>
        ))}
      </div>
    </div>
  );
}
