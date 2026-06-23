# 13. למידת מכונה ובינה מלאכותית במערכת

## 13.1 תהליך איסוף הנתונים

**המערכה משלבת שני מקורות ראשיים:**

1. **YOLO v8 (Computer Vision)**
   - מספור כלים בזמן אמת מתוך זרם ה-RTSP/וידאו
   - כל נתיב בצומת מעובד כ-ROI נפרד
   - פלט: `vehicle_count` לכל נתיב (lane_id)
   - פרמטרים: Confidence threshold ≥ 0.45 (כלים בלבד)

2. **סנסורים וידאו מדומים / ממשיים**
   - `intersection_vision.py` → `IntersectionState`: מצב צומת מלא
   - דוגמים מצב צומת בתדר 1 Hz כברירת מחדל
   - נתונים שנאספים: מספר כלים, זמני המתנה, צפיפות, אות חירום GPS

**דוגמה מצב משודר:**
```json
{
  "intersection_id": 1,
  "num_lanes": 4,
  "timestamp": 1718631234.567,
  "lanes": [
    {"lane_id": 0, "vehicle_count": 12, "density_pct": 55.0, "waiting_time_sec": 10.2},
    {"lane_id": 1, "vehicle_count": 5, "density_pct": 22.0, "waiting_time_sec": 3.1}
  ]
}
```

---

## 13.2 טיוב ואיזון נתונים

**עיבוד מקדים (Preprocessing):**
- **Normalization**: ספירות כלים → טווח [0, max_vehicles_per_lane]
- **Smoothing**: מיצוע נוסף על 3 דגימות עוקבות להקטנת רעש
- **Outlier Handling**: ערכים חרגים מ-±2σ מוחלפים בממוצע החלון
- **Traffic Thresholds**: כיול Waiting Time ל-3 רמות:
  - LOW: 0–6 שניות
  - MEDIUM: 6–15 שניות  
  - HIGH: 15+ שניות

**איזון וקטור ה-RL:**
- וקטור מצב דינאמי: `[vehicle_count_0, vehicle_count_1, ..., vehicle_count_N]`
- דיסקרטיזציה לשלוש רמות צפיפות (Low/Medium/High) לייצוב למידה

---

## 13.3 סוג הלמידה הנבחר

**Q-Learning (Reinforcement Learning)**

מודל: $Q(s, a) \leftarrow Q(s, a) + \alpha [r + \gamma \max_a Q(s', a) - Q(s, a)]$

**פרמטרים:**
- Learning Rate (α) = 0.10
- Discount Factor (γ) = 0.95
- Initial Exploration (ε₀) = 0.15
- ε-Decay = 0.995 (לכל צעד)
- Minimum ε = 0.02

**מנוע החלטות היברידי (Rule-Based + RL):**
1. **EmergencyRule**: אם אות חירום פעיל → בחר Phase שנותן ירוק לנתיב החירום
2. **MutualExclusionRule**: סינון Actions שמפרים סכסוכי נתיבים (מ-lane_conflicts)
3. **StarvationRule**: נתיב שמחכה >15 שניות קיבול עדיפות הגברה
4. **DeadlockRule**: אם אין Actions בטוחים → בחר פאזה לנתיב העמוס ביותר

---

## 13.4 נוסחאות ורשתות למידה

### Reward Function
$$r(s, a, s') = -\sum_{i=1}^{n} \text{waiting\_time}_i(s') + 50 \cdot \mathbb{1}[\text{emergency\_green}] - 20 \cdot \mathbb{1}[\text{conflict}]$$

- **משקל Waiting Time**: -1 לכל שנייה המתנה כוללת
- **Bonus חירום**: +50 אם רכב חירום קיבל ירוק
- **עונש סכסוך**: -20 אם פאזה מפרה Mutual Exclusion

### State Encoding
$$s = \text{hash}(\text{discretized\_lane\_counts} + \text{phase\_history})$$

כל מצב מנוהל ב-Q-table דינאמי עם אתחול אופטימיסטי (Q₀ = 10).

### Structure ה-Decision Tree
```cpp
DecisionTree {
  EmergencyRule   → if (emergency_active) return Phase_Emergency
  MutualExclusionRule → filter Actions to safe only
  StarvationRule  → boost starved lanes
  DeadlockRule    → fallback: pick highest-load lane
}
```

---

## 13.5 כיוונון, ייעול וניטור מדדי ביצוע

### A. כיוונון פרמטרים

| פרמטר | ערך | הערה |
|-------|-----|------|
| α (Learning Rate) | 0.10 | שמרני להתקרבות יציבה |
| γ (Discount) | 0.95 | וקטור עתידי משמעותי |
| ε (Exploration) | 0.15→0.02 | Decay לפי צעדים |
| Max Episodes | 30 min | להקראת Q-table |
| Thresholds | Configurable | מ-traffic_thresholds_*.json |

### B. ניטור ביצוע ב-Real-Time

**KPI מרכזיים:**
- **Average Waiting Time**: זמן המתנה ממוצע לכל נתיב
- **Throughput**: רכבים מעובדים בדקה
- **Phase Stability**: מספר החלפות פאז (נמוך = טוב)
- **Emergency Response Time**: זמן עד שרכב חירום קיבל ירוק

**ערכי התייחסות:**
- ✅ Baseline (fixed timing): ~12 sec avg wait
- ✅ RL (no coord): ~8–9 sec avg wait (25% שיפור)
- ✅ RL + Neighbor Coord: ~6–7 sec avg wait (40% שיפור)

### C. שמירה וטעינה של Q-table

```python
# Save after training
agent.saveQTable(f"qtable_intersection_{id}.tsv")

# Load at runtime
agent.loadQTable(f"qtable_intersection_{id}.tsv")
```

**מקום האחסון:** `cpp/qtable_intersection_*.tsv` (TSV format)

### D. Monitoring Dashboard

**מערכת בזמן אמת משדרת:**
- צמת נוכחי + מצב צומת (סטטוס צפיפות)
- Phase פעיל + זמן שהייה
- אות חירום (אם פעיל)
- מדדי רשת (Neighbor status, sync score)

**Granularity:**
- Live Updates: WebSocket בתדר 1 Hz
- KPI Reports: SQL Server כל 5 דקות
- Debug Logs: RLAgent decision trace (עם flags)

---

## סיכום שלב הלמידה

המערכה משתמשת בـ **Q-Learning + Rule-Based Tree** להכרעה מבוססת סטטיסטיקה וכללים אבטחה. YOLO מספק קלט ואמין מצלמות, התהליך מתכנס בתוך 30 דקות, וה-KPI חיובי משקפים שיפור של 25–40% ביחס לתיזום קבוע.

