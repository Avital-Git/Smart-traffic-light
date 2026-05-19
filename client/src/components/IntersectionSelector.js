export function IntersectionSelector({ intersections, selectedId, onChange }) {
  return (
    <div className="card">
      <h2>בחירת צומת</h2>
      <select className="select" value={selectedId ?? ''} onChange={(e) => onChange(Number(e.target.value))}>
        {intersections.map((intersection) => (
          <option key={intersection.id} value={intersection.id}>
            {intersection.code} - {intersection.name} ({intersection.city})
          </option>
        ))}
      </select>
    </div>
  );
}
