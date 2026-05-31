export function IntersectionVisual({ status, compact = false }) {
  if (!status) return null;

  const lanes = status.lanes || [];
  const directions = Array.isArray(status.laneDirections) ? status.laneDirections : [];

  const getLaneColor = (laneId) => {
    if (!status.currentPhase || status.currentPhase === 'Hold') {
      return 'RED';
    }
    const match = /^Phase(\d+)$/.exec(status.currentPhase);
    if (!match) {
      return 'RED';
    }
    const phaseId = Number(match[1]);
    const lanePhase = laneId % 2 === 0 ? 0 : 1;
    return lanePhase === phaseId ? 'GREEN' : 'RED';
  };

  const normalizeDirection = (value, laneId) => {
    const raw = String(value || '').trim().toUpperCase();
    const alias = {
      N: 'N', NORTH: 'N',
      S: 'S', SOUTH: 'S',
      E: 'E', EAST: 'E',
      W: 'W', WEST: 'W',
      NE: 'NE', NORTHEAST: 'NE',
      NW: 'NW', NORTHWEST: 'NW',
      SE: 'SE', SOUTHEAST: 'SE',
      SW: 'SW', SOUTHWEST: 'SW'
    };

    if (alias[raw]) {
      return alias[raw];
    }

    // Fallback keeps a stable real intersection shape even if direction is missing.
    return ['N', 'E', 'S', 'W'][laneId % 4];
  };

  const directionLabel = (token) => {
    if (token === 'N') return 'צפון';
    if (token === 'S') return 'דרום';
    if (token === 'E') return 'מזרח';
    if (token === 'W') return 'מערב';
    if (token === 'NE') return 'צפון-מזרח';
    if (token === 'NW') return 'צפון-מערב';
    if (token === 'SE') return 'דרום-מזרח';
    if (token === 'SW') return 'דרום-מערב';
    return token || 'נתיב';
  };

  const laneDirById = {};
  lanes.forEach((lane) => {
    laneDirById[lane.lane_id] = normalizeDirection(directions[lane.lane_id], lane.lane_id);
  });

  const lanesByDir = lanes.reduce((acc, lane) => {
    const key = laneDirById[lane.lane_id];
    if (!acc[key]) {
      acc[key] = [];
    }
    acc[key].push(lane);
    return acc;
  }, {});

  const knownTokens = ['N', 'S', 'E', 'W', 'NE', 'NW', 'SE', 'SW'];
  const extraTokens = Object.keys(lanesByDir).filter((t) => !knownTokens.includes(t));

  const groupStyleByDir = (token) => {
    const horizontal = token === 'N' || token === 'S' || token === 'NE' || token === 'NW' || token === 'SE' || token === 'SW';
    return {
      display: 'flex',
      flexDirection: horizontal ? 'row' : 'column',
      justifyContent: 'center',
      alignItems: 'center',
      flexWrap: 'wrap',
      gap: 12,
      width: '100%',
      maxWidth: '100%',
      minWidth: 0
    };
  };

  const laneCardStyleByDir = (token) => {
    const common = {
      position: 'static',
      transform: 'none',
      width: compact ? 'min(140px, 100%)' : 'min(220px, 100%)',
      flex: compact ? '1 1 136px' : '1 1 210px',
      minHeight: compact ? 240 : 280,
      maxWidth: '100%'
    };

    if (token === 'N' || token === 'S') {
      return {
        ...common,
        width: compact ? 'min(160px, 100%)' : 'min(240px, 100%)',
        flex: compact ? '1 1 154px' : '1 1 228px'
      };
    }

    // East/West cards need more width for text to fit
    if (token === 'E' || token === 'W') {
      return {
        ...common,
        width: compact ? 'min(140px, 100%)' : 'min(280px, 100%)',
        flex: compact ? '1 1 136px' : '1 1 270px'
      };
    }

    return common;
  };

  const cellStyle = {
    minHeight: compact ? 240 : 280,
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'center',
    width: '100%',
    minWidth: 0
  };

  const stageStyle = {
    position: 'relative',
    minHeight: 0,
    height: 'auto',
    overflow: 'visible',
    display: 'grid',
      gridTemplateColumns: compact
      ? 'minmax(110px, 1fr) minmax(200px, 1.4fr) minmax(110px, 1fr)'
      : 'minmax(0, 1fr) minmax(360px, 1.8fr) minmax(0, 1fr)',
    gridTemplateRows: compact ? 'auto minmax(220px, auto) auto' : 'auto minmax(340px, auto) auto',
    gap: compact ? 6 : 16,
    padding: compact ? '8px 6px' : '20px 12px',
    alignItems: 'stretch',
    width: '100%',
    boxSizing: 'border-box'
  };

  const renderDirectionGroup = (tokens) => {
    const dirLanes = tokens.flatMap((token) => lanesByDir[token] || []);
    if (dirLanes.length === 0) {
      return null;
    }
    const tokenForStyle = tokens[0];
    return (
      <div style={groupStyleByDir(tokenForStyle)}>
        {dirLanes.map((lane) => renderLaneCard(lane, laneDirById[lane.lane_id]))}
      </div>
    );
  };

  const renderLaneCard = (lane, dirToken) => {
    const signalColor = getLaneColor(lane.lane_id);
    const vehicleTotal = Math.max(0, Number(lane.vehicle_count || 0));
    const laneEmergencyActive = Boolean(status.emergencyActive) && Number(status.emergencyLaneId) === Number(lane.lane_id);
    const isGreen = signalColor === 'GREEN';

    const movementByDir = {
      N: 'down',
      S: 'up',
      E: 'left',
      W: 'right',
      NE: 'down',
      NW: 'down',
      SE: 'up',
      SW: 'up'
    };

    const roadOrientationByDir = {
      N: 'vertical',
      S: 'vertical',
      E: 'horizontal',
      W: 'horizontal',
      NE: 'vertical',
      NW: 'vertical',
      SE: 'vertical',
      SW: 'vertical'
    };

    const movement = movementByDir[dirToken] || 'down';
    const roadOrientation = roadOrientationByDir[dirToken] || 'horizontal';

    const carsToRender = Math.min(10, vehicleTotal);
    const animationDuration = Math.max(1.6, 4.4 - Math.min(vehicleTotal, 16) * 0.14);
    const laneWaitSec = Math.round(Number(lane.waiting_time_sec || 0));
    const roadTone = laneEmergencyActive ? 'road-emergency' : isGreen ? 'road-green' : 'road-red';

    const animationNameByMove = {
      right: isGreen ? 'driveThrough' : 'stopAtLight',
      left: isGreen ? 'driveThroughLeft' : 'stopAtLightLeft',
      down: isGreen ? 'driveThroughVertical' : 'stopAtLightVertical',
      up: isGreen ? 'driveThroughVerticalUp' : 'stopAtLightVerticalUp'
    };

    const animationName = animationNameByMove[movement] || (isGreen ? 'driveThrough' : 'stopAtLight');

    const renderVehicle = (index) => (
      <span
        key={`car-${lane.lane_id}-${index}`}
        className={[
          'emoji-car',
          `emoji-car--${roadOrientation}`,
          `emoji-car--${movement}`,
          isGreen ? 'emoji-car--green' : 'emoji-car--red'
        ].join(' ')}
        style={{
          '--car-delay': `${(index * animationDuration) / Math.max(1, carsToRender)}s`,
          '--car-duration': `${animationDuration}s`,
          '--stop-distance': `${Math.max(26, 78 - index * 8)}px`,
          '--drive-distance': `${roadOrientation === 'horizontal' ? 170 : 120}px`,
          animation: isGreen
            ? `${animationName} var(--car-duration) linear var(--car-delay) infinite`
            : `${animationName} calc(var(--car-duration) * 0.72) ease-out var(--car-delay) forwards`
        }}
      >
        {index % 2 === 0 ? '🚗' : '🚙'}
      </span>
    );

    return (
      <div
        key={lane.lane_id}
        className="intersection-lane"
        style={{
          ...laneCardStyleByDir(dirToken),
          borderColor: laneEmergencyActive ? '#dc2626' : undefined,
          boxShadow: laneEmergencyActive ? '0 0 0 2px rgba(220, 38, 38, 0.2)' : undefined,
          background: laneEmergencyActive ? '#fff7f7' : undefined,
          display: 'flex',
          flexDirection: 'column',
          gap: '8px'
        }}
      >
        <div className="lane-header">
          <span className={`lane-signal ${signalColor === 'GREEN' ? 'green' : 'red'}`} />
          <strong>{directionLabel(dirToken)}</strong>
          <span className="lane-live-badge">LIVE</span>
        </div>

        {laneEmergencyActive && !compact && (
          <div className="badge badge-danger" style={{ margin: '0 0 4px 0', padding: '3px 6px', fontSize: '10px' }}>
            חירום פעיל
          </div>
        )}

        <div className={`lane-meta ${compact ? 'lane-meta--compact' : ''}`}>
          <span className="lane-meta-item">🚗 {lane.vehicle_count}</span>
          <span className="lane-meta-item">⏱️ {laneWaitSec}s</span>
          <span className="lane-meta-item">🛣️ {lane.lane_id}</span>
        </div>

        <div className={`lane-road lane-road--${roadOrientation} ${roadTone}`} aria-label={`lane-${lane.lane_id}-sim`} style={{ flex: '1 1 auto', minHeight: compact ? 60 : 80 }}>
          <span className="lane-road-surface" />
          <span className="lane-divider lane-divider--primary" />
          <span className="lane-divider lane-divider--secondary" />
          <span className={`lane-stopline ${isGreen ? 'go' : 'stop'}`} />
          <span className="lane-intersection-glow" />

          {Array.from({ length: carsToRender }).map((_, index) => renderVehicle(index))}
        </div>
      </div>
    );
  };

  return (
    <div className="intersection-stage" style={stageStyle}>
      <div style={cellStyle}>{renderDirectionGroup(['NW'])}</div>
      <div style={cellStyle}>{renderDirectionGroup(['N'])}</div>
      <div style={cellStyle}>{renderDirectionGroup(['NE'])}</div>

      <div style={cellStyle}>{renderDirectionGroup(['W'])}</div>
      <div style={{ ...cellStyle, minHeight: compact ? 220 : 340 }}>
        <div
          className="intersection-cross"
          style={{
            position: 'static',
            transform: 'none',
            inset: 'auto',
            width: '100%',
            maxWidth: compact ? 200 : 450,
            height: compact ? 220 : 340
          }}
        />
      </div>
      <div style={cellStyle}>{renderDirectionGroup(['E'])}</div>

      <div style={cellStyle}>{renderDirectionGroup(['SW'])}</div>
      <div style={cellStyle}>{renderDirectionGroup(['S'])}</div>
      <div style={cellStyle}>{renderDirectionGroup(['SE'])}</div>

      {extraTokens.length > 0 && (
        <div style={{ gridColumn: '1 / -1', ...cellStyle, minHeight: compact ? 42 : 64 }}>
          {renderDirectionGroup(extraTokens)}
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
