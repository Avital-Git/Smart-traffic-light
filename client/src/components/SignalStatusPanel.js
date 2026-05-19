function colorClass(color) {
  if (color === 'GREEN') return 'green';
  if (color === 'YELLOW') return 'yellow';
  return 'red';
}

export function SignalStatusPanel({ status }) {
  if (!status) return null;

  return (
    <div className="card">
      <h2>מצב רמזורים בזמן אמת</h2>
      <div className="signal-list">
        {status.signals.map((signal) => (
          <div key={signal.direction} className="signal-item">
            <div className={`signal-light ${colorClass(signal.color)}`} />
            <div>
              <strong>{signal.direction}</strong>
              <div className="muted">
                {signal.color} | Queue: {signal.queue} | Wait: {signal.waitingSec}s
              </div>
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}
