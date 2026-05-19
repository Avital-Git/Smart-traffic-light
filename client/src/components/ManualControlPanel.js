import { useState } from 'react';

export function ManualControlPanel({ onSend }) {
  const [enabled, setEnabled] = useState(false);
  const [action, setAction] = useState('Phase0');

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
    </div>
  );
}
