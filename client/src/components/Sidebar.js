import { useState } from 'react';
import { NavLink } from 'react-router-dom';

const NAV_ITEMS = [
  { to: '/live', icon: '🟢', label: 'Live Dashboard' },
  { to: '/overview', icon: '📊', label: 'סקירה כללית' },
  { to: '/control',  icon: '🎛️', label: 'שליטה ידנית'  },
];

export function Sidebar({ selectedIntersection, connectionStatus }) {
  const [logoSrc, setLogoSrc] = useState('/logo.png');

  const handleLogoError = () => {
    if (logoSrc !== '/logo.svg') {
      setLogoSrc('/logo.svg');
    }
  };

  return (
    <aside className="sidebar">
      {/* ── Brand ── */}
      <div className="sidebar-brand">
        <img
          src={logoSrc}
          alt="Smart Traffic Logo"
          className="sidebar-logo-img"
          onError={handleLogoError}
        />
      </div>

      {/* ── Navigation ── */}
      <nav className="sidebar-nav">
        {NAV_ITEMS.map(({ to, icon, label }) => (
          <NavLink
            key={to}
            to={to}
            className={({ isActive }) =>
              `sidebar-link${isActive ? ' sidebar-link--active' : ''}`
            }
          >
            <span className="sidebar-icon" aria-hidden="true">{icon}</span>
            <span>{label}</span>
          </NavLink>
        ))}
      </nav>

      {/* ── Selected intersection card ── */}
      <div className="sidebar-section-title">צומת נבחרת</div>
      <div className="sidebar-intersection-card">
        {selectedIntersection ? (
          <>
            <div className="sidebar-intersection-name">
              {selectedIntersection.name}
            </div>
            <div className="sidebar-intersection-meta">
              מזהה&nbsp;#{selectedIntersection.id}
            </div>
          </>
        ) : (
          <div className="sidebar-intersection-empty">לא נבחרה צומת</div>
        )}
      </div>

      {/* ── Connection status ── */}
      <div className="sidebar-status-row">
        <span
          className={`sidebar-status-dot ${
            connectionStatus === 'online' ? 'dot-online' : 'dot-offline'
          }`}
        />
        <span className="sidebar-status-text">
          {connectionStatus === 'online' ? 'שרת מחובר' : 'שרת מנותק'}
        </span>
      </div>

      {/* ── Footer ── */}
      <div className="sidebar-footer">
        Smart Traffic v1.0&nbsp;·&nbsp;2026
      </div>
    </aside>
  );
}
