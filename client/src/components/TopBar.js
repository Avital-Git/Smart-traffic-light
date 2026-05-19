import { useLocation } from 'react-router-dom';

const PAGE_META = {
  '/live': {
    title: 'Live Dashboard',
    subtitle: 'עדכוני WebSocket, KPI רשת ומצב צמתים בזמן אמת',
  },
  '/overview': {
    title: 'סקירה כללית',
    subtitle: 'מצב רמזורים, עומסים ותרשימים בזמן אמת',
  },
  '/control': {
    title: 'שליטה ידנית',
    subtitle: 'שליחת פקודות פאזה ידנית לצמתים',
  },
};

export function TopBar({ selectedIntersection, connectionStatus }) {
  const { pathname } = useLocation();
  const meta = PAGE_META[pathname] ?? { title: 'Smart Traffic', subtitle: '' };

  return (
    <header className="top-bar">
      <div className="top-bar-page">
        <h1 className="top-bar-title">{meta.title}</h1>
        <p className="top-bar-subtitle">{meta.subtitle}</p>
      </div>

      <div className="top-bar-right">
        {selectedIntersection && (
          <div className="top-bar-intersection">
            <span className="top-bar-intersection-label">צומת:&nbsp;</span>
            <span className="top-bar-intersection-name">
              {selectedIntersection.name}
            </span>
          </div>
        )}

        <div
          className={`top-bar-badge ${
            connectionStatus === 'online' ? 'badge-online' : 'badge-offline'
          }`}
        >
          <span className="badge-dot" />
          {connectionStatus === 'online' ? 'מחובר' : 'מנותק'}
        </div>
      </div>
    </header>
  );
}
