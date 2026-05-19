export function TrafficStatusCard({ status }) {
  if (!status) return null;

  return (
    <div className="card">
      <h2>מצב תנועה בצומת</h2>
      <div className="stats-grid">
        <div className="stat-box">
          <span>פאזה נוכחית</span>
          <strong>{status.currentPhase}</strong>
        </div>
        <div className="stat-box">
          <span>רמת עומס</span>
          <strong>{status.congestionLevel}</strong>
        </div>
        <div className="stat-box">
          <span>סך תור</span>
          <strong>{status.totalQueue}</strong>
        </div>
        <div className="stat-box">
          <span>זמן המתנה ממוצע</span>
          <strong>{status.avgWaitSec} sec</strong>
        </div>
      </div>
      <p className="muted">עודכן לאחרונה: {status.updatedAt}</p>
    </div>
  );
}
