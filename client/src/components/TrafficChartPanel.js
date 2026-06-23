import { useEffect, useRef, useState } from 'react';

export function IntersectionVisual({ status, compact = false }) {
  if (!status) return null;

  const MIN_PHASE_HOLD_MS = 2200;

  const lanes = Array.isArray(status.lanes) ? status.lanes : [];
  const directions = Array.isArray(status.laneDirections)
    ? status.laneDirections
    : Array.isArray(status.skyDirections)
      ? status.skyDirections
      : [];
  const knownTokens = ['N', 'S', 'E', 'W'];

  const [displayPhase, setDisplayPhase] = useState(status.currentPhase || 'Hold');
  const phaseSinceRef = useRef(Date.now());
  const phaseTimerRef = useRef(null);

  useEffect(() => {
    const nextPhase = status.currentPhase || 'Hold';
    if (nextPhase === displayPhase) return undefined;

    const elapsed = Date.now() - phaseSinceRef.current;
    const remaining = Math.max(0, MIN_PHASE_HOLD_MS - elapsed);

    if (remaining === 0) {
      setDisplayPhase(nextPhase);
      phaseSinceRef.current = Date.now();
      return undefined;
    }

    if (phaseTimerRef.current) {
      window.clearTimeout(phaseTimerRef.current);
    }

    phaseTimerRef.current = window.setTimeout(() => {
      setDisplayPhase(nextPhase);
      phaseSinceRef.current = Date.now();
      phaseTimerRef.current = null;
    }, remaining);

    return () => {
      if (phaseTimerRef.current) {
        window.clearTimeout(phaseTimerRef.current);
        phaseTimerRef.current = null;
      }
    };
  }, [status.currentPhase, displayPhase]);

  const getLaneId = (lane) => {
    const id = lane?.lane_id ?? lane?.id;
    return Number.isFinite(Number(id)) ? Number(id) : -1;
  };

  const getLaneColor = (laneId) => {
    const hasServerEmergencyLane = Number.isFinite(Number(emergencyLaneFromStatus));
    if (hasServerEmergencyLane && emergencyActive && Number(laneId) === Number(emergencyLaneFromStatus)) {
      return '#10B981';
    }
    if (!displayPhase || displayPhase === 'Hold') {
      return '#EF4444';
    }
    const match = /^Phase(\d+)$/.exec(displayPhase);
    if (!match) {
      return '#EF4444';
    }
    const phaseId = Number(match[1]);
    const lanePhase = laneId % 2 === 0 ? 0 : 1;
    return lanePhase === phaseId ? '#10B981' : '#EF4444';
  };

  const normalizeDirection = (value, laneId) => {
    const raw = String(value || '').trim().toUpperCase();
    const alias = {
      N: 'N', NORTH: 'N',
      S: 'S', SOUTH: 'S',
      E: 'E', EAST: 'E',
      W: 'W', WEST: 'W',
      NE: 'N', NORTHEAST: 'N',
      NW: 'N', NORTHWEST: 'N',
      SE: 'S', SOUTHEAST: 'S',
      SW: 'S', SOUTHWEST: 'S'
    };
    if (alias[raw]) return alias[raw];
    return knownTokens[laneId % knownTokens.length];
  };

  const directionLabel = (token) => {
    if (token === 'N') return 'צפון';
    if (token === 'S') return 'דרום';
    if (token === 'E') return 'מזרח';
    if (token === 'W') return 'מערב';
    return token || 'נתיב';
  };

  const emergencySignal = status.emergency_signal || status.state?.emergency_signal || null;

  const emergencyLaneFromStatus = [
    status.emergencyLaneId,
    status.emergency_lane_id,
    emergencySignal?.lane_id,
    status.state?.emergency_signal?.lane_id,
  ].find((value) => Number.isFinite(Number(value)));

  const emergencyActive = Boolean(
    status.emergencyActive ||
    status.emergency_active ||
    emergencySignal?.active ||
    status.state?.emergency_signal?.active ||
    status.actionSource === 'emergency_preempt'
  );

  const laneDirById = {};
  lanes.forEach((lane, index) => {
    const laneId = getLaneId(lane);
    const directionFromLane = lane?.direction || lane?.lane_direction || lane?.geo_direction || lane?.cardinal_direction;
    const fromStatusMap = status.laneDirectionById?.[laneId] || status.lane_direction_by_id?.[laneId];
    laneDirById[laneId] = normalizeDirection(
      directionFromLane || fromStatusMap || directions[laneId] || directions[index],
      laneId < 0 ? index : laneId
    );
  });

  const lanesByDir = lanes.reduce((acc, lane) => {
    const laneId = getLaneId(lane);
    const key = laneDirById[laneId];
    if (!acc[key]) acc[key] = [];
    acc[key].push(lane);
    return acc;
  }, {});

  const lanesN = lanesByDir.N || [];
  const lanesS = lanesByDir.S || [];
  const lanesE = lanesByDir.E || [];
  const lanesW = lanesByDir.W || [];
  const hasN = lanesN.length > 0;
  const hasS = lanesS.length > 0;
  const hasE = lanesE.length > 0;
  const hasW = lanesW.length > 0;

  const renderLane = (lane, dirToken, laneIndex, totalInDirection) => {
    const laneId = getLaneId(lane);
    const signalColor = getLaneColor(laneId);
    const vehicleCount = Math.max(0, Number(lane?.vehicle_count || lane?.vehicleCount || 0));
    const waitingSec = Math.max(0, Math.round(Number(lane?.waiting_time_sec || lane?.waitingTimeSec || 0)));
    const carsVisualCount = Math.max(0, Math.min(3, vehicleCount));
    const axisClass = dirToken === 'N' || dirToken === 'S' ? 'iv-lane-axis-h' : 'iv-lane-axis-v';
    const dividerClass = laneIndex < totalInDirection - 1
      ? (axisClass === 'iv-lane-axis-h' ? 'iv-divider-right' : 'iv-divider-bottom')
      : '';
    const laneLocalEmergency = Boolean(lane?.emergencyActive || lane?.emergency_active);
    const hasServerEmergencyLane = Number.isFinite(Number(emergencyLaneFromStatus));
    const isEmergency = hasServerEmergencyLane
      ? Boolean(emergencyActive && Number(laneId) === Number(emergencyLaneFromStatus))
      : Boolean(emergencyActive && laneLocalEmergency);
    const isGreen = signalColor === '#10B981';

    return (
      <div
        key={`lane-${dirToken}-${laneId}`}
        className={`iv-lane iv-lane-${dirToken.toLowerCase()} ${axisClass} ${dividerClass} ${isEmergency ? 'is-emergency' : ''}`}
      >
        <span className="iv-lane-id">#{laneId}</span>
        <span className="iv-lane-stats" title="נתוני YOLO בזמן אמת">
          🚗 {vehicleCount} | ⏱️ {waitingSec}s
        </span>
        {carsVisualCount > 0 && (
          <span className={`iv-lane-flow iv-lane-flow-${dirToken.toLowerCase()}`} aria-label="lane-traffic-flow">
            {Array.from({ length: carsVisualCount }).map((_, i) => (
              <span
                key={`car-${laneId}-${i}`}
                className={`iv-car-icon ${isGreen ? 'is-moving' : 'is-stopped'}`}
                style={isGreen ? { animationDelay: `${i * 0.35}s` } : undefined}
              >
                🚘
              </span>
            ))}
          </span>
        )}
        <span className="iv-signal-shell">
          <span className="iv-signal-light" style={{ backgroundColor: signalColor, boxShadow: `0 0 14px ${signalColor}AA` }} />
          {isEmergency && <span className="iv-emergency-beacon" aria-label="emergency-lane-beacon" />}
        </span>
        {isEmergency && (
          <span className="iv-emergency-overlay" aria-label="emergency-lane-overlay">
            <span className="iv-emergency-overlay-icon">🚨</span>
            <span className="iv-emergency-overlay-text">🚑 רכב חירום</span>
          </span>
        )}
      </div>
    );
  };

  const renderRoad = (dirToken) => {
    const dirLanes = lanesByDir[dirToken] || [];
    const isHorizontal = dirToken === 'N' || dirToken === 'S';
    const dynamicGridStyle = dirLanes.length > 0
      ? {
          gridTemplateColumns: isHorizontal ? `repeat(${dirLanes.length}, minmax(0, 1fr))` : undefined,
          gridTemplateRows: !isHorizontal ? `repeat(${dirLanes.length}, minmax(0, 1fr))` : undefined,
        }
      : undefined;

    return (
      <div className={`iv-road iv-road-${dirToken.toLowerCase()}`}>
        <span className="iv-road-centerline" />
        <span className="iv-road-dashed iv-road-dashed-a" />
        <span className="iv-road-dashed iv-road-dashed-b" />

        <div className={`iv-lanes iv-lanes-${dirToken.toLowerCase()}`} style={dynamicGridStyle}>
          {dirLanes.map((lane, laneIndex) => renderLane(lane, dirToken, laneIndex, dirLanes.length))}
        </div>

        {!compact && <span className="iv-dir-label">{directionLabel(dirToken)}</span>}
      </div>
    );
  };

  return (
    <div className={`iv-stage ${compact ? 'iv-stage-compact' : ''}`}>
      <div
        className="iv-grid"
        style={{
          gridTemplateColumns: `${hasW ? '1fr' : '0fr'} minmax(170px, 1.45fr) ${hasE ? '1fr' : '0fr'}`,
          gridTemplateRows: `${hasN ? '1fr' : '0fr'} minmax(170px, 1.45fr) ${hasS ? '1fr' : '0fr'}`,
        }}
      >
        {hasN && <div className="iv-cell iv-north">{renderRoad('N')}</div>}
        {hasW && <div className="iv-cell iv-west">{renderRoad('W')}</div>}

        <div className="iv-core" aria-label="intersection-core">
          <span className="iv-core-lane-mark iv-core-mark-v" />
          <span className="iv-core-lane-mark iv-core-mark-h" />
        </div>

        {hasE && <div className="iv-cell iv-east">{renderRoad('E')}</div>}
        {hasS && <div className="iv-cell iv-south">{renderRoad('S')}</div>}
      </div>

      {lanes.length > 0 && (
        <div className="iv-legend">
          <span>🚗 {lanes.reduce((sum, lane) => sum + Math.max(0, Number(lane.vehicle_count || lane.vehicleCount || 0)), 0)}</span>
          <span>⏱️ {Math.round(lanes.reduce((sum, lane) => sum + Number(lane.waiting_time_sec || lane.waitingTimeSec || 0), 0))}s</span>
          {emergencyActive && <span className="iv-legend-emergency">מצב חירום פעיל</span>}
        </div>
      )}
    </div>
  );
}

export function TrafficChartPanel({ status }) {
  if (!status) return null;

  const lanes = status.lanes || [];

  return (
    <div className="card">
      <h2>תצוגת צומת בזמן אמת</h2>

      <IntersectionVisual status={status} />

      {lanes.length > 4 && <div className="intersection-extra-note" style={{ position: 'static', marginTop: 8 }}>מוצגים כל הנתיבים לפי כיווני DB.</div>}

      <p className="muted">עדכון חי לפי נתוני השרת מהצומת הנבחרת.</p>

      {Array.isArray(status.neighbors) && status.neighbors.length > 0 && (
        <div className="intersection-neighbors">
          <strong>כיוונים לפי מסד נתונים:</strong>
          <div className="intersection-neighbors-list">
            {status.neighbors.map((n, idx) => (
              <span key={`${n.adjacent_intersection_id || idx}-${idx}`} className="intersection-neighbor-chip">
                {n.direction_from || '?'} → {n.adjacent_name || `#${n.adjacent_intersection_id}`}
              </span>
            ))}
          </div>
        </div>
      )}
    </div>
  );
}
