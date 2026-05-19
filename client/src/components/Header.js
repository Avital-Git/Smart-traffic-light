import { NavLink } from 'react-router-dom';

export function Header() {
  return (
    <header className="page-header">
      <div>
        <h1>Smart Traffic Dashboard</h1>
        <p>בחירת צומת, מצב רמזורים בזמן אמת, עומסים, שליטה ידנית והתרעות חירום</p>
      </div>

      <nav className="page-nav">
        <NavLink
          to="/overview"
          className={({ isActive }) => `nav-link ${isActive ? 'active' : ''}`}
        >
          דף נתונים
        </NavLink>
        <NavLink
          to="/control"
          className={({ isActive }) => `nav-link ${isActive ? 'active' : ''}`}
        >
          דף שליטה
        </NavLink>
      </nav>
    </header>
  );
}
