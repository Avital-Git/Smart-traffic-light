import { useEffect, useMemo, useRef, useState } from 'react';
import {
  adminChangeUserPassword,
  adminClearEmergency,
  adminCreateUser,
  adminCreateLane,
  adminCreateIntersection,
  adminDeleteUser,
  adminDeleteLane,
  adminGetIntersection,
  adminListLanes,
  adminListIntersections,
  adminListUsers,
  adminLogin,
  adminManualControl,
  adminNeighbors,
  adminSendEmergency,
  adminUpdateLane,
  adminUpdateIntersection,
  clearAdminToken,
  getAdminToken,
  verifyAdminToken,
  adminGetLaneConflicts,
  adminCreateLaneConflict,
  adminDeleteLaneConflict,
  getIntersectionConflicts,
  adminGetIntersectionPhaseOptions
} from '../services/adminApi';

const EMPTY_FORM = {
  code: '',
  name: '',
  latitude: '',
  longitude: '',
  num_cameras: 4,
  city: '',
  region: '',
  description: ''
};

export function AdminPage() {
  const [loggedIn, setLoggedIn] = useState(false);
  const [currentAdminUsername, setCurrentAdminUsername] = useState('');
  const [currentAdminRole, setCurrentAdminRole] = useState('');
  const [loginForm, setLoginForm] = useState({ username: 'admin', password: '' });
  const [loginError, setLoginError] = useState('');

  const [intersections, setIntersections] = useState([]);
  const [selectedId, setSelectedId] = useState('');
  const [details, setDetails] = useState(null);
  const [neighbors, setNeighbors] = useState([]);
  const [lanes, setLanes] = useState([]);

  const [createForm, setCreateForm] = useState(EMPTY_FORM);
  const [updateForm, setUpdateForm] = useState({ name: '', num_cameras: '', city: '', region: '', description: '' });
  const [manualPhase, setManualPhase] = useState(0);
  const [manualSelectedLanes, setManualSelectedLanes] = useState([]);
  const [manualConflictError, setManualConflictError] = useState('');
  const [manualConflictPairs, setManualConflictPairs] = useState([]);
  const [manualPhaseOptions, setManualPhaseOptions] = useState([]);
  const [laneCreateForm, setLaneCreateForm] = useState({ camera_index: '', direction: 'N', description: '' });
  const [editingLaneId, setEditingLaneId] = useState(null);
  const [laneEditForm, setLaneEditForm] = useState({ direction: 'N', description: '' });
  const [selectedEmergencyLane, setSelectedEmergencyLane] = useState('');
  const [emergencyActive, setEmergencyActive] = useState(false);
  const emergencyTimerRef = useRef(null);

  // Lane conflicts state
  const [conflicts, setConflicts] = useState([]);
  const [conflictForm, setConflictForm] = useState({ lane_id_1: '', lane_id_2: '', conflict_type: 'crossing' });
  const [conflictValidationError, setConflictValidationError] = useState('');

  const [message, setMessage] = useState('');
  const [error, setError] = useState('');

  const [adminUsers, setAdminUsers] = useState([]);
  const [showAddUserForm, setShowAddUserForm] = useState(false);
  const [addUserForm, setAddUserForm] = useState({ username: '', password: '', confirmPassword: '' });
  const [editingPasswordUserId, setEditingPasswordUserId] = useState(null);
  const [passwordForm, setPasswordForm] = useState({ password: '', confirmPassword: '' });
  const [userMgmtMessage, setUserMgmtMessage] = useState('');
  const [userMgmtError, setUserMgmtError] = useState('');

  const selectedIntersection = useMemo(
    () => intersections.find((x) => String(x.intersection?.id || x.id) === String(selectedId)) || null,
    [intersections, selectedId]
  );

  useEffect(() => {
    const token = getAdminToken();
    if (!token) return;

    verifyAdminToken().then((result) => {
      if (result.ok) {
        setLoggedIn(true);
        setCurrentAdminUsername(result?.data?.username || '');
        setCurrentAdminRole(result?.data?.role || 'regular_admin');
      } else {
        clearAdminToken();
      }
    });
  }, []);

  useEffect(() => {
    if (!loggedIn) return;

    let isMounted = true;

    const loadUsers = async () => {
      const result = await adminListUsers();
      if (!isMounted) return;

      if (!result.ok) {
        setUserMgmtError(`טעינת משתמשים נכשלה: ${result.detail}`);
        return;
      }

      setAdminUsers(result.data.users || []);
      setUserMgmtError('');
    };

    loadUsers();

    return () => {
      isMounted = false;
    };
  }, [loggedIn]);

  useEffect(() => {
    if (!loggedIn) return;

    let isMounted = true;

    const load = async () => {
      const listResult = await adminListIntersections();
      if (!listResult.ok) {
        if (isMounted) setError(`טעינת צמתים נכשלה: ${listResult.detail}`);
        return;
      }

      if (!isMounted) return;
      setIntersections(listResult.data.intersections || []);

      if (!selectedId && (listResult.data.intersections || []).length > 0) {
        const first = listResult.data.intersections[0];
        setSelectedId(String(first.intersection?.id || first.id));
      }
    };

    load();
    const interval = window.setInterval(load, 8000);

    return () => {
      isMounted = false;
      window.clearInterval(interval);
    };
  }, [loggedIn, selectedId]);

  useEffect(() => () => {
    if (emergencyTimerRef.current) {
      clearTimeout(emergencyTimerRef.current);
      emergencyTimerRef.current = null;
    }
  }, []);

  useEffect(() => {
    if (!selectedId) {
      setSelectedEmergencyLane('');
      setManualSelectedLanes([]);
      setManualConflictError('');
      setManualConflictPairs([]);
      setManualPhaseOptions([]);
      return;
    }

    if ((lanes || []).length > 0) {
      const stillExists = lanes.some((lane) => String(lane.lane_id) === String(selectedEmergencyLane));
      if (!stillExists) {
        setSelectedEmergencyLane(String(lanes[0].lane_id));
      }
    } else {
      setSelectedEmergencyLane('');
    }
  }, [selectedId, lanes, selectedEmergencyLane]);

  useEffect(() => {
    if (!loggedIn || !selectedId) return;

    let isMounted = true;

    const normalizeConflictPairs = (publicConflictsPayload, adminConflictRows) => {
      if (Array.isArray(publicConflictsPayload)) {
        return publicConflictsPayload
          .map((pair) => [Number(pair?.[0]), Number(pair?.[1])])
          .filter((pair) => Number.isFinite(pair[0]) && Number.isFinite(pair[1]) && pair[0] !== pair[1]);
      }

      if (Array.isArray(adminConflictRows)) {
        return adminConflictRows
          .map((row) => [Number(row?.lane_id_1), Number(row?.lane_id_2)])
          .filter((pair) => Number.isFinite(pair[0]) && Number.isFinite(pair[1]) && pair[0] !== pair[1]);
      }

      return [];
    };

    const loadDetails = async () => {
      const [d, n, l, cAdmin, cPublic, p] = await Promise.all([
        adminGetIntersection(selectedId),
        adminNeighbors(selectedId),
        adminListLanes(selectedId),
        adminGetLaneConflicts(selectedId),
        getIntersectionConflicts(selectedId),
        adminGetIntersectionPhaseOptions(selectedId)
      ]);

      if (!isMounted) return;

      if (d.ok) {
        setDetails(d.data);
      }
      if (n.ok) {
        setNeighbors(n.data.neighbors || []);
      }
      if (l.ok) {
        setLanes(l.data.lanes || []);
      }
      if (cAdmin.ok) {
        const rows = Array.isArray(cAdmin.data.conflict_rows)
          ? cAdmin.data.conflict_rows
          : (cAdmin.data.conflicts || []);
        setConflicts(rows);
      } else if (cAdmin.ok === false) {
        setConflicts([]);
      }

      if (cPublic.ok) {
        setManualConflictPairs(normalizeConflictPairs(cPublic.data.conflicts, cAdmin.ok ? cAdmin.data.conflicts : []));
      } else {
        setManualConflictPairs(normalizeConflictPairs([], cAdmin.ok ? cAdmin.data.conflicts : []));
      }

      if (p.ok) {
        setManualPhaseOptions(Array.isArray(p.data.phase_options) ? p.data.phase_options : []);
      } else {
        setManualPhaseOptions([]);
      }
    };

    loadDetails();
    const interval = window.setInterval(loadDetails, 5000);

    return () => {
      isMounted = false;
      window.clearInterval(interval);
    };
  }, [loggedIn, selectedId]);

  useEffect(() => {
    // Remove lane selections that no longer exist after refresh
    const laneIds = new Set((lanes || []).map((l) => Number(l.lane_id)));
    setManualSelectedLanes((current) => current.filter((id) => laneIds.has(Number(id))));
  }, [lanes]);

  async function handleLogin(e) {
    e.preventDefault();
    setLoginError('');

    const result = await adminLogin(loginForm.username, loginForm.password);
    if (!result.ok) {
      setLoginError(result.detail || 'התחברות נכשלה');
      return;
    }

    setLoggedIn(true);
    const verifyResult = await verifyAdminToken();
    if (verifyResult.ok) {
      setCurrentAdminUsername(verifyResult?.data?.username || loginForm.username);
      setCurrentAdminRole(verifyResult?.data?.role || result.data?.role || 'regular_admin');
    } else {
      setCurrentAdminUsername(loginForm.username);
      setCurrentAdminRole(result.data?.role || 'regular_admin');
    }
    setMessage('התחברות מנהל הצליחה.');
  }

  function handleLogout() {
    clearAdminToken();
    setLoggedIn(false);
    setCurrentAdminUsername('');
    setCurrentAdminRole('');
    setDetails(null);
    setNeighbors([]);
    setLanes([]);
    setAdminUsers([]);
    setShowAddUserForm(false);
    setEditingPasswordUserId(null);
    setUserMgmtMessage('');
    setUserMgmtError('');
    setMessage('נותקת מחשבון המנהל.');
  }

  async function refreshAdminUsers() {
    const result = await adminListUsers();
    if (!result.ok) {
      setUserMgmtError(`טעינת משתמשים נכשלה: ${result.detail}`);
      return false;
    }
    setAdminUsers(result.data.users || []);
    setUserMgmtError('');
    return true;
  }

  async function handleCreateAdminUser(e) {
    e.preventDefault();
    setUserMgmtError('');
    setUserMgmtMessage('');

    const username = addUserForm.username.trim();
    const password = addUserForm.password;
    const confirmPassword = addUserForm.confirmPassword;

    if (!username) {
      setUserMgmtError('יש להזין שם משתמש.');
      return;
    }
    if (password !== confirmPassword) {
      setUserMgmtError('הסיסמה ואימות הסיסמה אינם תואמים.');
      return;
    }

    const result = await adminCreateUser({ username, password, confirm_password: confirmPassword });
    if (!result.ok) {
      setUserMgmtError(`יצירת משתמש נכשלה: ${result.detail}`);
      return;
    }

    setAddUserForm({ username: '', password: '', confirmPassword: '' });
    setShowAddUserForm(false);
    setUserMgmtMessage(`המשתמש ${username} נוצר בהצלחה.`);
    await refreshAdminUsers();
  }

  async function handleDeleteAdminUser(userId) {
    setUserMgmtError('');
    setUserMgmtMessage('');

    const userToDelete = (adminUsers || []).find((u) => Number(u.user_id) === Number(userId));
    const usernameToDelete = userToDelete?.username || `#${userId}`;
    const confirmed = window.confirm(`האם למחוק את המשתמש ${usernameToDelete}? פעולה זו אינה הפיכה.`);
    if (!confirmed) {
      return;
    }

    const result = await adminDeleteUser(userId);
    if (!result.ok) {
      setUserMgmtError(`מחיקת משתמש נכשלה: ${result.detail}`);
      return;
    }

    setUserMgmtMessage('המשתמש נמחק בהצלחה.');
    await refreshAdminUsers();
  }

  function startChangePassword(userId) {
    setEditingPasswordUserId(userId);
    setPasswordForm({ password: '', confirmPassword: '' });
    setUserMgmtError('');
    setUserMgmtMessage('');
  }

  function cancelChangePassword() {
    setEditingPasswordUserId(null);
    setPasswordForm({ password: '', confirmPassword: '' });
  }

  async function handleChangeUserPassword(userId) {
    setUserMgmtError('');
    setUserMgmtMessage('');

    if (passwordForm.password !== passwordForm.confirmPassword) {
      setUserMgmtError('הסיסמה החדשה ואימות הסיסמה אינם תואמים.');
      return;
    }

    const result = await adminChangeUserPassword(userId, {
      password: passwordForm.password,
      confirm_password: passwordForm.confirmPassword
    });
    if (!result.ok) {
      setUserMgmtError(`עדכון סיסמה נכשל: ${result.detail}`);
      return;
    }

    cancelChangePassword();
    setUserMgmtMessage('הסיסמה עודכנה בהצלחה.');
    await refreshAdminUsers();
  }

  function formatAdminDate(value) {
    if (!value) return 'Γאפ';
    const d = new Date(value);
    if (Number.isNaN(d.getTime())) return value;
    return d.toLocaleString('he-IL');
  }

  async function handleCreate(e) {
    e.preventDefault();
    setError('');
    setMessage('');

    const payload = {
      ...createForm,
      latitude: Number(createForm.latitude),
      longitude: Number(createForm.longitude),
      num_cameras: Number(createForm.num_cameras)
    };

    const result = await adminCreateIntersection(payload);
    if (!result.ok) {
      setError(`יצירת צומת נכשלה: ${result.detail}`);
      return;
    }

    setMessage(`הצומת ${payload.code} נוצרה בהצלחה.`);
    setCreateForm(EMPTY_FORM);

    const listResult = await adminListIntersections();
    if (listResult.ok) {
      setIntersections(listResult.data.intersections || []);
    }
  }

  async function handleUpdate(e) {
    e.preventDefault();
    if (!selectedId) return;

    const payload = {};
    Object.entries(updateForm).forEach(([k, v]) => {
      if (v === '' || v == null) return;
      payload[k] = k === 'num_cameras' ? Number(v) : v;
    });

    if (Object.keys(payload).length === 0) {
      setError('יש להזין לפחות שדה אחד לעדכון.');
      return;
    }

    const result = await adminUpdateIntersection(selectedId, payload);
    if (!result.ok) {
      setError(`עדכון צומת נכשל: ${result.detail}`);
      return;
    }

    setMessage(`צומת #${selectedId} עודכנה בהצלחה.`);
    setUpdateForm({ name: '', num_cameras: '', city: '', region: '', description: '' });

    const detailResult = await adminGetIntersection(selectedId);
    if (detailResult.ok) {
      setDetails(detailResult.data);
    }
  }

  async function handleManualControl() {
    if (!selectedId) return;

    const result = await adminManualControl(selectedId, manualPhase, 'admin_dashboard_manual_override');
    if (!result.ok) {
      setError(`שליטה ידנית נכשלה: ${result.detail}`);
      return;
    }

    setMessage(`הופעלה פאזה ידנית ${manualPhase} לצומת #${selectedId}.`);
  }

  function findConflictPairForSelection(selectedLaneIds) {
    if (!Array.isArray(selectedLaneIds) || selectedLaneIds.length < 2) return null;

    const selectedSet = new Set(selectedLaneIds.map((x) => Number(x)));
    for (const pair of (manualConflictPairs || [])) {
      const lane1 = Number(pair[0]);
      const lane2 = Number(pair[1]);
      if (selectedSet.has(lane1) && selectedSet.has(lane2)) {
        return [lane1, lane2];
      }
    }
    return null;
  }

  function laneWouldConflictWithSelection(laneId, selectedLaneIds) {
    const numericLane = Number(laneId);
    const selectedSet = new Set((selectedLaneIds || []).map((x) => Number(x)));
    for (const pair of (manualConflictPairs || [])) {
      const lane1 = Number(pair[0]);
      const lane2 = Number(pair[1]);
      if ((lane1 === numericLane && selectedSet.has(lane2)) || (lane2 === numericLane && selectedSet.has(lane1))) {
        return [lane1, lane2];
      }
    }
    return null;
  }

  function resolvePhaseFromSelectedLanes(selectedLaneIds) {
    if (!Array.isArray(selectedLaneIds) || selectedLaneIds.length === 0) return null;

    const normalizedSelection = [...selectedLaneIds]
      .map((x) => Number(x))
      .filter((x) => Number.isFinite(x))
      .sort((a, b) => a - b);

    for (const option of (manualPhaseOptions || [])) {
      const lanes = Array.isArray(option.green_lanes)
        ? option.green_lanes.map((x) => Number(x)).filter((x) => Number.isFinite(x)).sort((a, b) => a - b)
        : [];

      if (lanes.length !== normalizedSelection.length) continue;
      let same = true;
      for (let i = 0; i < lanes.length; i += 1) {
        if (lanes[i] !== normalizedSelection[i]) {
          same = false;
          break;
        }
      }
      if (same) {
        return Number(option.phase_id);
      }
    }

    return null;
  }

  function handleToggleManualLane(laneId) {
    const numericLane = Number(laneId);
    setManualConflictError('');

    if (manualSelectedLanes.includes(numericLane)) {
      setManualSelectedLanes((current) => current.filter((id) => id !== numericLane));
      return;
    }

    const conflictPair = laneWouldConflictWithSelection(numericLane, manualSelectedLanes);
    if (conflictPair) {
      setManualConflictError(`נתיבים ${conflictPair[0]} ו-${conflictPair[1]} לא יכולים להידלק יחד`);
      return;
    }

    setManualSelectedLanes((current) => [...current, numericLane]);
  }

  async function handleManualLaneControlSubmit() {
    if (!selectedId) return;

    if (!manualSelectedLanes.length) {
      setError('יש לבחור לפחות נתיב אחד להפעלה ידנית.');
      return;
    }

    const conflictPair = findConflictPairForSelection(manualSelectedLanes);
    if (conflictPair) {
      setManualConflictError(`נתיבים ${conflictPair[0]} ו-${conflictPair[1]} לא יכולים להידלק יחד`);
      setError('נחסמה שליחה: השילוב הנבחר מכיל קונפליקט.');
      return;
    }

    const mappedPhase = resolvePhaseFromSelectedLanes(manualSelectedLanes);
    if (mappedPhase == null) {
      const available = (manualPhaseOptions || [])
        .map((opt) => `${opt.label || `Phase${opt.phase_id}`}: [${(opt.green_lanes || []).join(',')}]`)
        .join(' | ');
      setError(`לא ניתן למפות את שילוב הנתיבים לפאזה חוקית מהשרת. פאזות זמינות: ${available || 'אין'}`);
      return;
    }

    const result = await adminManualControl(
      selectedId,
      mappedPhase,
      `manual_lane_selection:${manualSelectedLanes.join(',')}`
    );
    if (!result.ok) {
      setError(`שליטה ידנית לפי נתיבים נכשלה: ${result.detail}`);
      return;
    }

    setMessage(`הפעלה ידנית בוצעה לפי נתיבים [${manualSelectedLanes.join(', ')}] Γזע Phase${mappedPhase}.`);
  }

  async function reloadLanes() {
    if (!selectedId) return;
    const result = await adminListLanes(selectedId);
    if (result.ok) {
      setLanes(result.data.lanes || []);
    }
  }

  async function handleCreateLane(e) {
    e.preventDefault();
    if (!selectedId) return;

    const payload = {
      camera_index: Number(laneCreateForm.camera_index),
      direction: String(laneCreateForm.direction || '').toUpperCase(),
      description: laneCreateForm.description || null,
    };

    const result = await adminCreateLane(selectedId, payload);
    if (!result.ok) {
      setError(`הוספת נתיב נכשלה: ${result.detail}`);
      return;
    }

    setMessage(`נוסף נתיב חדש לצומת #${selectedId}`);
    setLaneCreateForm({ camera_index: '', direction: 'N', description: '' });
    await reloadLanes();
  }

  function startEditLane(lane) {
    setEditingLaneId(lane.lane_id);
    setLaneEditForm({
      direction: lane.direction || 'N',
      description: lane.description || '',
    });
  }

  function cancelEditLane() {
    setEditingLaneId(null);
    setLaneEditForm({ direction: 'N', description: '' });
  }

  async function saveEditLane(laneId) {
    if (!selectedId) return;
    const payload = {
      direction: String(laneEditForm.direction || '').toUpperCase(),
      description: laneEditForm.description,
    };

    const result = await adminUpdateLane(selectedId, laneId, payload);
    if (!result.ok) {
      setError(`עדכון נתיב נכשל: ${result.detail}`);
      return;
    }

    setMessage(`נתיב ${laneId} עודכן בהצלחה.`);
    cancelEditLane();
    await reloadLanes();
  }

  async function handleDeleteLane(laneId) {
    if (!selectedId) return;
    const result = await adminDeleteLane(selectedId, laneId);
    if (!result.ok) {
      setError(`מחיקת נתיב נכשלה: ${result.detail}`);
      return;
    }

    setMessage(`נתיב ${laneId} נמחק.`);
    await reloadLanes();
  }

  async function handleSendEmergencySignal() {
    if (!selectedId || selectedEmergencyLane === '') return;

    const sendResult = await adminSendEmergency(selectedId, selectedEmergencyLane, 'AMB001');
    if (!sendResult.ok) {
      setError(`שליחת אות חירום נכשלה: ${sendResult.detail}`);
      return;
    }

    setEmergencyActive(true);
    setMessage(`אות חירום נשלח לנתיב ${selectedEmergencyLane}.`);

    if (emergencyTimerRef.current) {
      clearTimeout(emergencyTimerRef.current);
    }

    emergencyTimerRef.current = setTimeout(async () => {
      const clearResult = await adminClearEmergency(selectedId);
      if (!clearResult.ok) {
        setError(`ניקוי חירום אוטומטי נכשל: ${clearResult.detail}`);
      } else {
        setMessage('חירום הסתיים');
      }
      setEmergencyActive(false);
      emergencyTimerRef.current = null;
    }, 30000);
  }

  async function handleCreateConflict(e) {
    e.preventDefault();
    if (!selectedId) return;

    setConflictValidationError('');

    const lane1 = Number(conflictForm.lane_id_1);
    const lane2 = Number(conflictForm.lane_id_2);

    // Validation: cannot conflict with itself
    if (lane1 === lane2) {
      setConflictValidationError('נתיב לא יכול להיות בסכסוך עם עצמו');
      return;
    }

    // Check if lanes are selected
    if (!lane1 || !lane2) {
      setConflictValidationError('יש בחור שני נתיבים');
      return;
    }

    // Check if conflict already exists
    const alreadyExists = conflicts.some(
      (c) =>
        (c.lane_id_1 === lane1 && c.lane_id_2 === lane2) ||
        (c.lane_id_1 === lane2 && c.lane_id_2 === lane1)
    );
    if (alreadyExists) {
      setConflictValidationError('סכסוך זה כבר קיים');
      return;
    }

    const result = await adminCreateLaneConflict(
      selectedId,
      lane1,
      lane2,
      conflictForm.conflict_type
    );
    if (!result.ok) {
      setError(`הוספת סכסוך נתיבים נכשלה: ${result.detail}`);
      return;
    }

    setMessage(`סכסוך בין נתיבים ${lane1} ו-${lane2} נוסף בהצלחה.`);
    setConflictForm({ lane_id_1: '', lane_id_2: '', conflict_type: 'crossing' });
    const updatedRows = result.data.conflicts || [];
    setConflicts(updatedRows);
    setManualConflictPairs(
      updatedRows
        .map((row) => [Number(row?.lane_id_1), Number(row?.lane_id_2)])
        .filter((pair) => Number.isFinite(pair[0]) && Number.isFinite(pair[1]) && pair[0] !== pair[1])
    );
  }

  async function handleDeleteConflict(conflictId) {
    if (!selectedId) return;

    const result = await adminDeleteLaneConflict(selectedId, conflictId);
    if (!result.ok) {
      setError(`מחיקת סכסוך נכשלה: ${result.detail}`);
      return;
    }

    setMessage(`סכסוך נמחק בהצלחה.`);
    const updatedRows = result.data.conflicts || [];
    setConflicts(updatedRows);
    setManualConflictPairs(
      updatedRows
        .map((row) => [Number(row?.lane_id_1), Number(row?.lane_id_2)])
        .filter((pair) => Number.isFinite(pair[0]) && Number.isFinite(pair[1]) && pair[0] !== pair[1])
    );
  }

  function getLaneDirection(laneId) {
    const lane = lanes.find((l) => l.lane_id === Number(laneId));
    return lane ? lane.direction : laneId;
  }

  if (!loggedIn) {
    return (
      <div className="admin-auth-wrap">
        <form className="card admin-auth-card" onSubmit={handleLogin}>
          <h2>כניסת מנהל</h2>
          <p className="muted">נדרש אימות כדי לבצע פעולות ניהול.</p>

          <input
            className="input"
            placeholder="שם משתמש"
            value={loginForm.username}
            onChange={(e) => setLoginForm((cur) => ({ ...cur, username: e.target.value }))}
          />
          <input
            className="input"
            type="password"
            placeholder="סיסמה"
            value={loginForm.password}
            onChange={(e) => setLoginForm((cur) => ({ ...cur, password: e.target.value }))}
          />

          {loginError && <div className="error-banner" style={{ padding: 10 }}>{loginError}</div>}

          <button className="button primary" type="submit">התחבר כמנהל</button>
        </form>
      </div>
    );
  }

  return (
    <>
      <div className="top-row">
        <div className="card compact">
          <h2>לוח ניהול</h2>
          <p>ניהול צמתים, מעקב זמן אמת ושליטה ידנית.</p>
          {message && <div className="message">{message}</div>}
          {error && <div className="error-banner" style={{ padding: 10, marginTop: 8 }}>{error}</div>}
        </div>
        <div className="card compact">
          <h2>חשבון מנהל</h2>
          <p>מחובר כעת ומאומת מול השרת.</p>
          <p className="muted" style={{ marginBottom: 8 }}>
            משתמש: <strong>{currentAdminUsername || 'Γאפ'}</strong>
          </p>
          <button className="button" onClick={handleLogout}>התנתק</button>
        </div>
      </div>

      <div className="admin-grid">
        <div className="card">
          <h3>בחירת צומת</h3>
          <select className="select" value={selectedId} onChange={(e) => setSelectedId(e.target.value)}>
            {(intersections || []).map((item) => {
              const row = item.intersection || item;
              return (
                <option key={row.id} value={row.id}>
                  #{row.id} - {row.name}
                </option>
              );
            })}
          </select>

          <h3 style={{ marginTop: 16 }}>שליטה ידנית</h3>
          <select className="select" value={manualPhase} onChange={(e) => setManualPhase(Number(e.target.value))}>
            <option value={0}>Phase0</option>
            <option value={1}>Phase1</option>
            <option value={2}>Phase2</option>
            <option value={3}>Phase3</option>
          </select>
          <button className="button primary" onClick={handleManualControl}>הפעל שליטה ידנית</button>

          <h4 style={{ marginTop: 14, marginBottom: 8 }}>בחירה ידנית לפי נתיבים</h4>
          <p className="muted" style={{ marginBottom: 8 }}>
            בחירה זו נאכפת לפי קונפליקטים מהשרת ({`GET /intersection/${selectedId || ':id'}/conflicts`}).
          </p>

          {manualConflictError && (
            <div className="error-banner" style={{ padding: 8, marginBottom: 8, fontSize: 13, background: '#fee2e2', color: '#991b1b' }}>
              {manualConflictError}
            </div>
          )}

          <div style={{ display: 'grid', gap: 8 }}>
            {(lanes || []).length === 0 && <div className="muted">אין נתיבים זמינים לצומת.</div>}
            {(lanes || []).map((lane) => {
              const laneId = Number(lane.lane_id);
              const isSelected = manualSelectedLanes.includes(laneId);
              const disabledPair = !isSelected ? laneWouldConflictWithSelection(laneId, manualSelectedLanes) : null;
              const isBlocked = Boolean(disabledPair);

              return (
                <button
                  key={lane.lane_id}
                  type="button"
                  onClick={() => handleToggleManualLane(laneId)}
                  disabled={!isSelected && isBlocked}
                  style={{
                    textAlign: 'right',
                    borderRadius: 10,
                    border: isSelected ? '1px solid #16a34a' : '1px solid #cbd5e1',
                    background: isSelected ? '#dcfce7' : (isBlocked ? '#f1f5f9' : '#ffffff'),
                    color: isBlocked ? '#94a3b8' : '#0f172a',
                    opacity: isBlocked ? 0.65 : 1,
                    padding: '8px 10px',
                    cursor: (!isSelected && isBlocked) ? 'not-allowed' : 'pointer'
                  }}
                  title={isBlocked && disabledPair ? `נתיבים ${disabledPair[0]} ו-${disabledPair[1]} לא יכולים להידלק יחד` : ''}
                >
                  <strong>Lane #{lane.lane_id}</strong> ┬╖ {lane.direction}
                  {isBlocked && disabledPair && (
                    <span style={{ fontSize: 12, marginInlineStart: 6, color: '#dc2626' }}>
                      (חסום: {disabledPair[0]}Γזפ{disabledPair[1]})
                    </span>
                  )}
                </button>
              );
            })}
          </div>

          <div style={{ marginTop: 8, fontSize: 12, color: '#64748b' }}>
            פאזות זמינות מהשרת:{' '}
            {(manualPhaseOptions || []).length > 0
              ? manualPhaseOptions.map((opt) => `${opt.label || `Phase${opt.phase_id}`}[${(opt.green_lanes || []).join(',')}]`).join(' | ')
              : 'אין'}
          </div>

          <button
            className="button primary"
            type="button"
            style={{ marginTop: 10 }}
            onClick={handleManualLaneControlSubmit}
            disabled={(lanes || []).length === 0 || manualSelectedLanes.length === 0}
          >
            הפעל שילוב נתיבים
          </button>
        </div>

        <form className="card" onSubmit={handleCreate}>
          <h3>הוספת צומת חדשה</h3>
          <input className="input" placeholder="קוד צומת" value={createForm.code} onChange={(e) => setCreateForm((c) => ({ ...c, code: e.target.value }))} required />
          <input className="input" placeholder="שם צומת" value={createForm.name} onChange={(e) => setCreateForm((c) => ({ ...c, name: e.target.value }))} required />
          <div className="admin-two-col">
            <input className="input" type="number" step="0.000001" placeholder="Latitude" value={createForm.latitude} onChange={(e) => setCreateForm((c) => ({ ...c, latitude: e.target.value }))} required />
            <input className="input" type="number" step="0.000001" placeholder="Longitude" value={createForm.longitude} onChange={(e) => setCreateForm((c) => ({ ...c, longitude: e.target.value }))} required />
          </div>
          <input className="input" type="number" min={1} max={16} placeholder="מס' מצלמות" value={createForm.num_cameras} onChange={(e) => setCreateForm((c) => ({ ...c, num_cameras: e.target.value }))} />
          <input className="input" placeholder="עיר" value={createForm.city} onChange={(e) => setCreateForm((c) => ({ ...c, city: e.target.value }))} />
          <input className="input" placeholder="אזור" value={createForm.region} onChange={(e) => setCreateForm((c) => ({ ...c, region: e.target.value }))} />
          <textarea className="textarea" placeholder="תיאור" value={createForm.description} onChange={(e) => setCreateForm((c) => ({ ...c, description: e.target.value }))} />
          <button className="button primary" type="submit">הוסף צומת</button>
        </form>

        <form className="card" onSubmit={handleUpdate}>
          <h3>עדכון צומת קיימת</h3>
          <p className="muted">ניתן לעדכן כל שדה. שדות ריקים לא יעודכנו.</p>
          <input className="input" placeholder="שם חדש" value={updateForm.name} onChange={(e) => setUpdateForm((c) => ({ ...c, name: e.target.value }))} />
          <input className="input" type="number" min={1} max={16} placeholder="מס' מצלמות" value={updateForm.num_cameras} onChange={(e) => setUpdateForm((c) => ({ ...c, num_cameras: e.target.value }))} />
          <input className="input" placeholder="עיר" value={updateForm.city} onChange={(e) => setUpdateForm((c) => ({ ...c, city: e.target.value }))} />
          <input className="input" placeholder="אזור" value={updateForm.region} onChange={(e) => setUpdateForm((c) => ({ ...c, region: e.target.value }))} />
          <textarea className="textarea" placeholder="תיאור" value={updateForm.description} onChange={(e) => setUpdateForm((c) => ({ ...c, description: e.target.value }))} />
          <button className="button" type="submit">עדכן צומת</button>
        </form>
      </div>

      <div className="admin-grid" style={{ marginTop: 16 }}>
        <div className="card">
          <h3>מצב אמת - צומת נבחרת</h3>
          {details?.intersection ? (
            <div className="admin-live-box">
              <div><strong>שם:</strong> {details.intersection.name}</div>
              <div><strong>קוד:</strong> {details.intersection.intersection_code || selectedIntersection?.intersection?.code || '-'}</div>
              <div><strong>פאזה נוכחית:</strong> {details.current_action?.action || 'לא זמין'}</div>
              <div><strong>מספר נתיבים:</strong> {details.current_state?.num_lanes || 'לא זמין'}</div>
              <div><strong>עודכן:</strong> {details.current_state?.timestamp ? new Date(details.current_state.timestamp * 1000).toLocaleString('he-IL') : 'לא זמין'}</div>
            </div>
          ) : (
            <div className="muted">אין נתונים זמינים לצומת.</div>
          )}
        </div>

        <div className="card">
          <h3>סימולציית שכנים דינמית</h3>
          <p className="muted">מספר הסימולציות מותאם אוטומטית למספר השכנים של הצומת.</p>
          <div className="neighbor-grid">
            {(neighbors || []).length === 0 && <div className="muted">לצומת זו אין שכנים כרגע.</div>}
            {(neighbors || []).map((neighbor) => (
              <div className="neighbor-card" key={neighbor.intersection_id}>
                <h4>צומת שכנה #{neighbor.intersection_id}</h4>
                <div>פאזה: {neighbor.action?.action || 'לא זמין'}</div>
                <div>נתיבים: {neighbor.state?.num_lanes || 'לא זמין'}</div>
                <div>תורים: {(neighbor.state?.lanes || []).reduce((sum, lane) => sum + (lane.vehicle_count || 0), 0)}</div>
                <div>חירום: {neighbor.state?.emergency_signal?.active ? 'כן' : 'לא'}</div>
              </div>
            ))}
          </div>
        </div>

        <div className="card">
          <h3>ניהול נתיבי צומת (intersection_lanes)</h3>
          <p className="muted">הוספה, עריכה ומחיקה של נתיבים בצומת הנבחרת.</p>

          <form onSubmit={handleCreateLane}>
            <div className="admin-two-col">
              <input
                className="input"
                type="number"
                min={0}
                placeholder="camera_index"
                value={laneCreateForm.camera_index}
                onChange={(e) => setLaneCreateForm((cur) => ({ ...cur, camera_index: e.target.value }))}
                required
              />
              <select
                className="select"
                value={laneCreateForm.direction}
                onChange={(e) => setLaneCreateForm((cur) => ({ ...cur, direction: e.target.value }))}
              >
                {['N', 'S', 'E', 'W', 'NE', 'NW', 'SE', 'SW'].map((d) => (
                  <option key={d} value={d}>{d}</option>
                ))}
              </select>
            </div>
            <input
              className="input"
              placeholder="description"
              value={laneCreateForm.description}
              onChange={(e) => setLaneCreateForm((cur) => ({ ...cur, description: e.target.value }))}
            />
            <button className="button primary" type="submit">הוסף נתיב</button>
          </form>

          <div style={{ marginTop: 14, display: 'grid', gap: 10 }}>
            {(lanes || []).length === 0 && <div className="muted">לא נמצאו נתיבים לצומת זו.</div>}
            {(lanes || []).map((lane) => {
              const isEditing = editingLaneId === lane.lane_id;
              return (
                <div key={lane.lane_id} className="neighbor-card">
                  <div><strong>Lane #{lane.lane_id}</strong> ┬╖ camera_index={lane.camera_index}</div>

                  {isEditing ? (
                    <>
                      <div className="admin-two-col">
                        <select
                          className="select"
                          value={laneEditForm.direction}
                          onChange={(e) => setLaneEditForm((cur) => ({ ...cur, direction: e.target.value }))}
                        >
                          {['N', 'S', 'E', 'W', 'NE', 'NW', 'SE', 'SW'].map((d) => (
                            <option key={d} value={d}>{d}</option>
                          ))}
                        </select>
                        <input
                          className="input"
                          placeholder="description"
                          value={laneEditForm.description}
                          onChange={(e) => setLaneEditForm((cur) => ({ ...cur, description: e.target.value }))}
                        />
                      </div>
                      <div style={{ display: 'flex', gap: 8 }}>
                        <button className="button primary" type="button" onClick={() => saveEditLane(lane.lane_id)}>
                          שמור
                        </button>
                        <button className="button" type="button" onClick={cancelEditLane}>
                          בטל
                        </button>
                      </div>
                    </>
                  ) : (
                    <>
                      <div>direction: <strong>{lane.direction}</strong></div>
                      <div>description: {lane.description || 'Γאפ'}</div>
                      <div style={{ display: 'flex', gap: 8 }}>
                        <button className="button" type="button" onClick={() => startEditLane(lane)}>ערוך</button>
                        <button className="button danger" type="button" onClick={() => handleDeleteLane(lane.lane_id)}>מחק</button>
                      </div>
                    </>
                  )}
                </div>
              );
            })}
          </div>
        </div>

        <div className="card">
          <h3>בקרת חירום</h3>
          <p className="muted">שליחת אות חירום לנתיב ספציפי בצומת הנבחרת.</p>

          <select
            className="select"
            value={selectedEmergencyLane}
            onChange={(e) => setSelectedEmergencyLane(e.target.value)}
            disabled={(lanes || []).length === 0 || emergencyActive}
          >
            {(lanes || []).length === 0 && <option value="">אין נתיבים זמינים</option>}
            {(lanes || []).map((lane) => (
              <option key={lane.lane_id} value={lane.lane_id}>
                Lane #{lane.lane_id} ┬╖ cam {lane.camera_index} ┬╖ {lane.direction}
              </option>
            ))}
          </select>

          <button
            className="button danger"
            type="button"
            onClick={handleSendEmergencySignal}
            disabled={(lanes || []).length === 0 || selectedEmergencyLane === '' || emergencyActive}
          >
            שלח אות חירום
          </button>

          {emergencyActive && (
            <div className="badge badge-danger" style={{ marginTop: 10 }}>
              חירום פעיל Γאפ ינוקה אוטומטית תוך 30 שניות
            </div>
          )}
        </div>

        <div className="card">
          <h3>ניהול קונפליקטים בין נתיבים</h3>
          <p className="muted">הגדרת זוגות נתיבים שלא יכולים להיות במצב ירוק בו זמנית.</p>

          {conflictValidationError && (
            <div className="error-banner" style={{ padding: 8, marginBottom: 10, fontSize: 13 }}>
              {conflictValidationError}
            </div>
          )}

          <form onSubmit={handleCreateConflict}>
            <div className="admin-two-col">
              <select
                className="select"
                value={conflictForm.lane_id_1}
                onChange={(e) => setConflictForm((cur) => ({ ...cur, lane_id_1: e.target.value }))}
              >
                <option value="">בחר נתיב 1</option>
                {(lanes || []).map((lane) => (
                  <option key={lane.lane_id} value={lane.lane_id}>
                    Lane #{lane.lane_id} ┬╖ {lane.direction}
                  </option>
                ))}
              </select>
              <select
                className="select"
                value={conflictForm.lane_id_2}
                onChange={(e) => setConflictForm((cur) => ({ ...cur, lane_id_2: e.target.value }))}
              >
                <option value="">בחר נתיב 2</option>
                {(lanes || []).map((lane) => (
                  <option key={lane.lane_id} value={lane.lane_id}>
                    Lane #{lane.lane_id} ┬╖ {lane.direction}
                  </option>
                ))}
              </select>
            </div>
            <select
              className="select"
              value={conflictForm.conflict_type}
              onChange={(e) => setConflictForm((cur) => ({ ...cur, conflict_type: e.target.value }))}
            >
              <option value="crossing">crossing (חציה)</option>
              <option value="merging">merging (המזגה)</option>
              <option value="diverging">diverging (התפצלות)</option>
            </select>
            <button className="button primary" type="submit">הוסף קונפליקט</button>
          </form>

          <div style={{ marginTop: 14, display: 'grid', gap: 10 }}>
            {(conflicts || []).length === 0 && (
              <div className="muted">אין קונפליקטים מוגדרים לצומת זו.</div>
            )}
            {(conflicts || []).map((conflict) => (
              <div key={conflict.conflict_id} className="neighbor-card">
                <div>
                  <strong>Lane #{conflict.lane_id_1}</strong> ({getLaneDirection(conflict.lane_id_1)})
                  {' '}
                  Γזפ
                  {' '}
                  <strong>Lane #{conflict.lane_id_2}</strong> ({getLaneDirection(conflict.lane_id_2)})
                </div>
                <div style={{ fontSize: 12, color: '#64748b', marginTop: 4 }}>
                  סוג: <strong>{conflict.conflict_type}</strong>
                  {conflict.created_at && (
                    <>
                      {' '} ┬╖ יוצר: {new Date(conflict.created_at).toLocaleString('he-IL')}
                    </>
                  )}
                </div>
                <button
                  className="button danger"
                  type="button"
                  onClick={() => handleDeleteConflict(conflict.conflict_id)}
                  style={{ marginTop: 8 }}
                >
                  מחק קונפליקט
                </button>
              </div>
            ))}
          </div>
        </div>
      </div>

      <div className="admin-grid" style={{ marginTop: 16 }}>
        <div className="card">
          <h3>ניהול משתמשי מערכת</h3>
          <p className="muted">ניהול משתמשי אדמין למערכת.</p>

          {userMgmtMessage && <div className="message" style={{ marginBottom: 10 }}>{userMgmtMessage}</div>}
          {userMgmtError && <div className="error-banner" style={{ padding: 10, marginBottom: 10 }}>{userMgmtError}</div>}

          {currentAdminRole === 'super_admin' && (
          <div style={{ marginBottom: 10 }}>
            <button
              className="button primary"
              type="button"
              onClick={() => {
                setShowAddUserForm((v) => !v);
                setAddUserForm({ username: '', password: '', confirmPassword: '' });
                setUserMgmtError('');
              }}
            >
              {showAddUserForm ? 'סגור טופס הוספה' : 'הוסף משתמש חדש'}
            </button>
          </div>
          )}

          {showAddUserForm && (
            <form onSubmit={handleCreateAdminUser} style={{ marginBottom: 14 }}>
              <div className="admin-two-col">
                <input
                  className="input"
                  placeholder="שם משתמש"
                  value={addUserForm.username}
                  onChange={(e) => setAddUserForm((cur) => ({ ...cur, username: e.target.value }))}
                  required
                />
                <input
                  className="input"
                  type="password"
                  placeholder="סיסמה"
                  value={addUserForm.password}
                  onChange={(e) => setAddUserForm((cur) => ({ ...cur, password: e.target.value }))}
                  required
                />
              </div>
              <input
                className="input"
                type="password"
                placeholder="אימות סיסמה"
                value={addUserForm.confirmPassword}
                onChange={(e) => setAddUserForm((cur) => ({ ...cur, confirmPassword: e.target.value }))}
                required
              />
              <button className="button primary" type="submit">שמור משתמש</button>
            </form>
          )}

          <div style={{ overflowX: 'auto' }}>
            <table style={{ width: '100%', borderCollapse: 'collapse' }}>
              <thead>
                <tr>
                  <th style={{ textAlign: 'right', borderBottom: '1px solid #e2e8f0', padding: '8px' }}>שם משתמש</th>
                  <th style={{ textAlign: 'right', borderBottom: '1px solid #e2e8f0', padding: '8px' }}>נוצר בתאריך</th>
                  <th style={{ textAlign: 'right', borderBottom: '1px solid #e2e8f0', padding: '8px' }}>התחברות אחרונה</th>
                  <th style={{ textAlign: 'right', borderBottom: '1px solid #e2e8f0', padding: '8px' }}>פעולות</th>
                </tr>
              </thead>
              <tbody>
                {(adminUsers || []).length === 0 && (
                  <tr>
                    <td colSpan={4} style={{ padding: '10px', color: '#64748b' }}>אין משתמשים להצגה.</td>
                  </tr>
                )}
                {(adminUsers || []).map((u) => (
                  <tr key={u.user_id}>
                    <td style={{ borderBottom: '1px solid #f1f5f9', padding: '8px' }}>{u.username}</td>
                    <td style={{ borderBottom: '1px solid #f1f5f9', padding: '8px' }}>{formatAdminDate(u.created_at)}</td>
                    <td style={{ borderBottom: '1px solid #f1f5f9', padding: '8px' }}>{formatAdminDate(u.last_login)}</td>
                    <td style={{ borderBottom: '1px solid #f1f5f9', padding: '8px' }}>
                      <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
                        {(currentAdminRole === 'super_admin' || u.username === currentAdminUsername) && (
                          <button className="button" type="button" onClick={() => startChangePassword(u.user_id)}>
                            שינוי סיסמה
                          </button>
                        )}
                        {currentAdminRole === 'super_admin' && u.username !== currentAdminUsername && (
                          <button className="button danger" type="button" onClick={() => handleDeleteAdminUser(u.user_id)}>
                            מחק
                          </button>
                        )}
                      </div>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>

          {editingPasswordUserId != null && (
            <div style={{ marginTop: 14, paddingTop: 12, borderTop: '1px solid #e2e8f0' }}>
              <h4 style={{ marginBottom: 8 }}>שינוי סיסמה למשתמש #{editingPasswordUserId}</h4>
              <div className="admin-two-col">
                <input
                  className="input"
                  type="password"
                  placeholder="סיסמה חדשה"
                  value={passwordForm.password}
                  onChange={(e) => setPasswordForm((cur) => ({ ...cur, password: e.target.value }))}
                />
                <input
                  className="input"
                  type="password"
                  placeholder="אימות סיסמה חדשה"
                  value={passwordForm.confirmPassword}
                  onChange={(e) => setPasswordForm((cur) => ({ ...cur, confirmPassword: e.target.value }))}
                />
              </div>
              <div style={{ display: 'flex', gap: 8 }}>
                <button className="button primary" type="button" onClick={() => handleChangeUserPassword(editingPasswordUserId)}>
                  עדכן סיסמה
                </button>
                <button className="button" type="button" onClick={cancelChangePassword}>
                  בטל
                </button>
              </div>
            </div>
          )}
        </div>
      </div>
    </>
  );
}
