# Smart Traffic Controller (C++)

מנוע החלטות תנועה ב-C++ במסלול אחיד אחד בלבד:

- `Junction` — ניהול מצב צומת, פאזות, מינימום/מקסימום ירוק וחירום
- `RLAgent` — Q-learning (בחירת פעולה, עדכון Q, reward)
- `Simulation` — השוואת Baseline מול RL
- `main.cpp` — שני מצבי הרצה: `--simulate` ו-`--server`

## Run

1. Configure + build

```bash
cd cpp
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

2. Simulation

```bash
.\build\Release\smart_traffic_controller.exe --simulate
```

3. Connected mode (FastAPI)

```bash
.\build\Release\smart_traffic_controller.exe --server 127.0.0.1 8000
```

4. Self tests

```bash
.\build\Release\smart_traffic_controller.exe --selftest
```

## Notes

- הקוד אוחד למסלול RL יחיד (`Junction + RLAgent`) ללא מסלול Q-learning ישן.
- ברירת מחדל ללא דגל מריצה סימולציה ומדפיסה גם הוראות שימוש.
- מעבר פאזה כולל guard בטיחותי של `yellow + all-red` (inter-green), ולכן אין דילוג על פרק מעבר בטוח גם בבקשת חירום.

## Traffic thresholds (runtime config)

הספים של עומס/המתנה/צפיפות **לא מקודדים בקוד** ונטענים מקובץ JSON בזמן ריצה.

פורמט לדוגמה:

```json
{
	"vehicle_count": { "light_max": 5, "medium_max": 15 },
	"waiting_time_sec": { "short_max": 20, "medium_max": 60 },
	"density_pct": { "low_max": 30, "medium_max": 70 }
}
```

סדר טעינה:

1. משתנה סביבה `TRAFFIC_THRESHOLDS_FILE` (override גלובלי)
2. קובץ פר-צומת: `traffic_thresholds_<intersection_id>.json`
3. קובץ פר-צומת תחת `cpp/`: `cpp/traffic_thresholds_<intersection_id>.json`
4. קובץ כללי: `traffic_thresholds.json` או `cpp/traffic_thresholds.json`
5. אם אין קובץ תקין — ברירות מחדל סבירות מתוך הקוד

במצב `--server`, כאשר מזהה הצומת משתנה, נטענת קונפיגורציית thresholds מתאימה לאותה צומת ללא קומפילציה מחדש.

## Phase safety / lane conflicts (runtime config)

אכיפת בטיחות לפאזות מתבצעת לפי קובץ JSON של זוגות נתיבים מתנגשים.
אם זוג נתיבים מסומן כ-conflict, הם לא יכולים לקבל ירוק בו-זמנית באותה פאזה.

פורמט לדוגמה:

```json
{
	"conflicts": [[0, 1], [0, 3], [2, 1], [2, 3]]
}
```

סדר טעינה:

1. משתנה סביבה `TRAFFIC_CONFLICTS_FILE` (override גלובלי)
2. קובץ פר-צומת: `lane_conflicts_<intersection_id>.json`
3. קובץ פר-צומת תחת `cpp/`: `cpp/lane_conflicts_<intersection_id>.json`
4. קובץ כללי: `lane_conflicts.json` או `cpp/lane_conflicts.json`
5. אם אין קובץ תקין — רשימת conflicts ריקה (אין חסימות נוספות מעבר לפאזות המוגדרות)

במצב `--server`, כאשר מזהה הצומת משתנה, נטענת גם קונפיגורציית conflicts מתאימה לאותה צומת.

## Neighbor coordination

קיים כעת מנגנון בסיסי של תיאום בין צמתים סמוכים:

- שרת ה-FastAPI מחזיר ב-`/intersection/{id}/packet` גם summaries של צמתים שכנים.
- טופולוגיית שכנים נטענת מ-`python/server/neighbor_topology.json`.
- הבקר ב-C++ משתמש במידע זה כדי:
	- להוסיף context ל-state encoding של ה-Q-table
	- להעדיף פאזה מסונכרנת עם שכן עמוס
	- להעניק bonus/penalty ב-reward לפי סנכרון או התנגדות לזרימה שכנה

זהו מימוש בסיסי של green-wave / adjacent coordination, אך עדיין לא תחליף למערכת רשת מלאה עם deadlock prevention גלובלי.

### Neighbor message authentication

השרת חותם כל `neighbor summary` עם `HMAC-SHA256` והבקר ב-C++ מאמת את החתימה לפני שימוש בנתון.

- קובץ קונפיג צד שרת: `python/server/neighbor_message_auth.json`
- קובץ קונפיג צד C++: `cpp/neighbor_message_auth.json`

Neighbor summary שלא עומד באימות חתימה/חלון זמן נדחה ולא נכנס ל-state.

### Neighbor tuning config + sweep

משקלי התיאום השכני נטענים מקובצי JSON חיצוניים (ללא קומפילציה מחדש), לדוגמה:

- `cpp/neighbor_tuning_conservative.json`
- `cpp/neighbor_tuning_balanced.json`
- `cpp/neighbor_tuning_aggressive.json`

במצב `--simulate`, המערכת מריצה sweep אוטומטי על פרופילי neighbor ומדפיסה את הפרופיל הטוב ביותר לפי ציון KPI משוקלל.

## Phase topology config (runtime)

ניתן להגדיר פאזות לכל צומת באמצעות JSON חיצוני במקום בנייה קשיחה של even/odd.

פורמט לדוגמה:

```json
{
	"intersections": {
		"1": {
			"phases": [
				{ "phase_id": 0, "green_lanes": [0, 2] },
				{ "phase_id": 1, "green_lanes": [1, 3] }
			]
		}
	}
}
```

סדר טעינה:

1. משתנה סביבה `TRAFFIC_PHASES_FILE`
2. `traffic_phases.json`
3. `cpp/traffic_phases.json`

התנהגות:

- אם נמצאו פאזות תקינות לצומת — הבקר ישתמש בהן.
- אם אין קונפיג מתאים/תקין — יש fallback אוטומטי ל-builder הישן (even/odd).
