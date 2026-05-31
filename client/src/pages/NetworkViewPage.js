import { useEffect, useMemo, useState } from 'react';
import { getIntersectionStatus, subscribeIntersectionUpdates } from '../services/api';
import { IntersectionVisual } from '../components/TrafficChartPanel';

function normalizeDirection(value) {
  const raw = String(value || '').trim().toUpperCase();
  if (['N', 'S', 'E', 'W', 'NE', 'NW', 'SE', 'SW'].includes(raw)) return raw;
  if (raw === 'NORTH') return 'N';
  if (raw === 'SOUTH') return 'S';
  if (raw === 'EAST') return 'E';
  if (raw === 'WEST') return 'W';
  return 'N';
}

function dirLabel(dir) {
  if (dir === 'N') return 'צפון';
  if (dir === 'S') return 'דרום';
  if (dir === 'E') return 'מזרח';
  if (dir === 'W') return 'מערב';
  if (dir === 'NE') return 'צפון-מזרח';
  if (dir === 'NW') return 'צפון-מערב';
  if (dir === 'SE') return 'דרום-מזרח';
  if (dir === 'SW') return 'דרום-מערב';
  return dir;
}

function NetworkFlowOverlay({ flowItems }) {
  if (!Array.isArray(flowItems) || flowItems.length === 0) {
    return null;
  }

  const endPointByDir = {
    N: { x: 50, y: 19 },
    S: { x: 50, y: 81 },
    E: { x: 81, y: 50 },
    W: { x: 19, y: 50 },
    NE: { x: 74, y: 26 },
    NW: { x: 26, y: 26 },
    SE: { x: 74, y: 74 },
    SW: { x: 26, y: 74 },
  };

  return (
    <svg className="network-flow-overlay" viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">
      <defs>
        <marker id="network-flow-arrow-head" markerWidth="6" markerHeight="6" refX="5" refY="3" orient="auto">
          <path d="M0,0 L6,3 L0,6 Z" fill="#3b82f6" opacity="0.8" />
        </marker>
      </defs>

      {flowItems.map(({ dir, count }) => {
        const end = endPointByDir[dir];
        if (!end) return null;
        const pathId = `flow-path-${dir}`;
        const dots = Math.max(1, Math.min(8, Math.round(count / 2)));
        const duration = Math.max(1.2, 4.2 - Math.min(count, 20) * 0.12);

        return (
          <g key={dir}>
            <path
              id={pathId}
              d={`M 50 50 L ${end.x} ${end.y}`}
              fill="none"
              stroke="#60a5fa"
              strokeWidth="0.85"
              opacity="0.72"
              markerEnd="url(#network-flow-arrow-head)"
            />

            {Array.from({ length: dots }).map((_, i) => (
              <circle key={`${dir}-${i}`} r="0.8" fill="#2563eb" opacity="0.95">
                <animateMotion
                  dur={`${duration}s`}
                  begin={`${(i * duration) / dots}s`}
                  repeatCount="indefinite"
                  rotate="auto"
                  path={`M 50 50 L ${end.x} ${end.y}`}
                />
              </circle>
            ))}
          </g>
        );
      })}
    </svg>
  );
}

function IntersectionNode({ intersection, status, isCenter, directionTag }) {
  return (
    <div className={`network-node-wrapper ${isCenter ? 'network-node-wrapper--center' : ''}`}>
      <div className="network-node-header">
        <div className="network-node-title">{intersection?.name || `צומת #${status?.intersectionId || '-'}`}</div>
        {!isCenter && <span className="network-dir-tag">{directionTag}</span>}
      </div>

      <div className="network-node-visual">
        {status ? <IntersectionVisual status={status} compact /> : <div className="network-node-loading">טוען...</div>}
      </div>
    </div>
  );
}

export function NetworkViewPage({ intersections, selectedIntersection, status }) {
  const [statusByIntersection, setStatusByIntersection] = useState({});

  const centerId = selectedIntersection?.id || status?.intersectionId || null;
  const neighbors = Array.isArray(status?.neighbors) ? status.neighbors : [];

  useEffect(() => {
    if (!centerId || !status) return;
    setStatusByIntersection((cur) => ({ ...cur, [centerId]: status }));
  }, [centerId, status]);

  useEffect(() => {
    if (!centerId) return undefined;

    const neighborIds = neighbors
      .map((n) => Number(n?.adjacent_intersection_id))
      .filter((id) => Number.isFinite(id));

    const uniqueIds = Array.from(new Set([centerId, ...neighborIds]));
    const unsubscribers = [];
    let isMounted = true;

    uniqueIds.forEach((intersectionId) => {
      getIntersectionStatus(intersectionId).then((nextStatus) => {
        if (!isMounted || !nextStatus) return;
        setStatusByIntersection((cur) => ({ ...cur, [intersectionId]: nextStatus }));
      });

      unsubscribers.push(
        subscribeIntersectionUpdates(intersectionId, (nextStatus) => {
          setStatusByIntersection((cur) => ({ ...cur, [intersectionId]: nextStatus }));
        })
      );
    });

    return () => {
      isMounted = false;
      unsubscribers.forEach((u) => {
        try {
          u();
        } catch {
          // ignore
        }
      });
    };
  }, [centerId, neighbors]);

  const intersectionById = useMemo(() => {
    const out = {};
    (intersections || []).forEach((item) => {
      out[item.id] = item;
    });
    return out;
  }, [intersections]);

  const neighborsByDirection = useMemo(() => {
    const buckets = {
      N: [],
      S: [],
      E: [],
      W: [],
      NE: [],
      NW: [],
      SE: [],
      SW: [],
      EXTRA: [],
    };

    neighbors.forEach((n) => {
      const dir = normalizeDirection(n?.direction_from);
      const adjacentId = Number(n?.adjacent_intersection_id);
      const entry = { ...n, dir, adjacentId };
      if (buckets[dir]) {
        buckets[dir].push(entry);
      } else {
        buckets.EXTRA.push(entry);
      }
    });

    return buckets;
  }, [neighbors]);

  const cells = useMemo(() => ([
    { key: 'NW', dirs: ['NW'] },
    { key: 'N', dirs: ['N'] },
    { key: 'NE', dirs: ['NE'] },
    { key: 'W', dirs: ['W'] },
    { key: 'CENTER', dirs: [] },
    { key: 'E', dirs: ['E'] },
    { key: 'SW', dirs: ['SW'] },
    { key: 'S', dirs: ['S'] },
    { key: 'SE', dirs: ['SE'] },
  ]), []);

  const flowItems = useMemo(() => {
    const dirs = ['N', 'S', 'E', 'W', 'NE', 'NW', 'SE', 'SW'];
    return dirs
      .map((dir) => {
        const list = neighborsByDirection[dir] || [];
        if (list.length === 0) return null;

        const count = list.reduce((sum, neighbor) => {
          const neighborStatus = statusByIntersection[neighbor.adjacentId];
          if (!neighborStatus) return sum;

          const laneSum = Array.isArray(neighborStatus.lanes)
            ? neighborStatus.lanes.reduce((laneAcc, lane) => laneAcc + Number(lane.vehicle_count || 0), 0)
            : 0;
          return sum + laneSum;
        }, 0);

        return { dir, count };
      })
      .filter(Boolean)
      .filter((item) => item.count > 0);
  }, [neighborsByDirection, statusByIntersection]);

  if (!centerId) {
    return (
      <div className="card">
        <h2>תצוגת רשת</h2>
        <p className="muted">בחר צומת כדי להציג רשת שכנים.</p>
      </div>
    );
  }

  return (
    <div className="card network-page-card">
      <h2>תצוגת רשת</h2>
      <p className="muted">הצומת הנבחרת במרכז, השכנים סביב לפי direction_from מהמסד.</p>

      <div className="network-canvas-wrap">
        <div className="network-grid-map">
          <NetworkFlowOverlay flowItems={flowItems} />

          {cells.map((cell) => {
            if (cell.key === 'CENTER') {
              return (
                <div key={cell.key} className="network-grid-cell network-grid-cell--center">
                  <IntersectionNode
                    intersection={intersectionById[centerId] || selectedIntersection}
                    status={statusByIntersection[centerId] || status}
                    isCenter
                  />
                </div>
              );
            }

            const items = cell.dirs.flatMap((d) => neighborsByDirection[d] || []);
            return (
              <div key={cell.key} className="network-grid-cell">
                {items.map((neighbor, idx) => {
                  const neighborId = neighbor.adjacentId;
                  const neighborStatus = statusByIntersection[neighborId] || null;
                  return (
                    <IntersectionNode
                      key={`${cell.key}-${neighborId}-${idx}`}
                      intersection={intersectionById[neighborId] || { name: neighbor.adjacent_name }}
                      status={neighborStatus}
                      directionTag={dirLabel(neighbor.dir)}
                    />
                  );
                })}
              </div>
            );
          })}
        </div>

        {neighborsByDirection.EXTRA.length > 0 && (
          <div className="network-extra-row">
            {neighborsByDirection.EXTRA.map((neighbor, idx) => {
              const neighborId = neighbor.adjacentId;
              const neighborStatus = statusByIntersection[neighborId] || null;
              return (
                <IntersectionNode
                  key={`extra-${neighborId}-${idx}`}
                  intersection={intersectionById[neighborId] || { name: neighbor.adjacent_name }}
                  status={neighborStatus}
                  directionTag={dirLabel(neighbor.dir)}
                />
              );
            })}
          </div>
        )}

        {(neighborsByDirection.N.length + neighborsByDirection.S.length + neighborsByDirection.E.length + neighborsByDirection.W.length + neighborsByDirection.NE.length + neighborsByDirection.NW.length + neighborsByDirection.SE.length + neighborsByDirection.SW.length) === 0 && (
          <div className="muted" style={{ padding: 14 }}>אין שכנים להצגה עבור צומת זו.</div>
        )}
      </div>
    </div>
  );
}
