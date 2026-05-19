import {
  CartesianGrid,
  Legend,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis
} from 'recharts';

export function TrafficChartPanel({ status }) {
  if (!status) return null;

  return (
    <div className="card">
      <h2>גרף עומסים</h2>
      <div className="chart-wrap">
        <ResponsiveContainer width="100%" height="100%">
          <LineChart data={status.chart}>
            <CartesianGrid strokeDasharray="3 3" />
            <XAxis dataKey="time" />
            <YAxis />
            <Tooltip />
            <Legend />
            <Line type="monotone" dataKey="queue" stroke="#2563eb" strokeWidth={2} />
            <Line type="monotone" dataKey="avgWaitSec" stroke="#dc2626" strokeWidth={2} />
          </LineChart>
        </ResponsiveContainer>
      </div>
    </div>
  );
}
