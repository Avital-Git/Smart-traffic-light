# Gap Report — Smart Traffic Project

מסמך זה מסכם את מצב הפרויקט בפועל מול הדרישות והיעדים שהוגדרו במסמך ההצעה.

## סיכום מנהלים

הפרויקט נמצא בשלב **אב-טיפוס עובד ומתקדם**:
- קיימת שרשרת עבודה פעילה של איסוף מצב צומת, העברת state לשרת, קבלת החלטה, ובקר C++ עם סימולציה.
- קיימת תמיכה דינאמית במספר נתיבים, עדיפות לרכב חירום, RL בסיסי, קונפיגורציה דינאמית של thresholds, ואכיפת conflict pairs לפאזות.
- עדיין חסרים רכיבים מהותיים כדי לעמוד **במלוא** ההצעה המקורית, בעיקר: תקשורת אמיתית בין צמתים, אבטחת מידע, לוגיקת הולכי רגל, מודל פאזות/טופולוגיה עשיר יותר, ובדיקות/הערכה פורמליות.

---

## 1. דרישות ליבה של המערכת

| דרישה מההצעה | סטטוס | מה קיים בפועל | מה עדיין חסר |
|---|---|---|---|
| מערכת ניהול תנועה חכמה בזמן אמת | Partial | קיימת שרשרת State → Decision → Action דרך [python/server/app.py](../python/server/app.py), [cpp/main.cpp](../cpp/main.cpp), [cpp/RLAgent.cpp](../cpp/RLAgent.cpp) | אין עדיין הוכחה/מדידה מסודרת של זמן אמת תחת עומס אמיתי |
| התאמה דינאמית למצב התנועה | Done | קבלת vehicle count, density, waiting time והחלטה דינאמית בצד C++/Server | נדרש טיוב נוסף של policy ו-KPI |
| תמיכה בצמתים עם מספר נתיבים משתנה | Done | מיושם ב-[python/auto_launcher.py](../python/auto_launcher.py), [python/vision/intersection_vision.py](../python/vision/intersection_vision.py), [cpp/main.cpp](../cpp/main.cpp) | - |
| זיהוי רכבי חירום ומתן קדימות | Partial | יש מודל `emergency_signal`, בחירת פאזה מתאימה ו-reward shaping | אין אינטגרציית GPS אמיתית, אין אימות מקור האות |
| תקשורת בין צמתים סמוכים | Partial | השרת מחזיר summaries של שכנים לפי טופולוגיה, והבקר ב-C++ צורך אותם בהחלטה וב-reward | עדיין אין פרוטוקול מבוזר מלא או סנכרון רשת רחב |
| מניעת פקק שרשרת / green wave | Partial | קיים מנגנון בסיסי של phase alignment מול שכנים עמוסים | אין עדיין deadlock prevention גלובלי או אופטימיזציה אזורית מלאה |
| שליטה מרחוק דרך אתר | Partial | יש backend ו-API לפעולת controller action, ויש שכבת API ב-frontend | לא נבדק כמערכת ניהול מלאה, אין workflow מלא ומתועד |

---

## 2. עיבוד תמונה וחישה

| דרישה מההצעה | סטטוס | מה קיים בפועל | מה עדיין חסר |
|---|---|---|---|
| מצלמה בכל נתיב | Partial | יש `IntersectionAnalyzer` שמייצר מצב פר-נתיב דינאמי | בסימולציה/אב-טיפוס אין פריסה אמיתית של מצלמה לכל נתיב |
| זיהוי מספר כלי רכב | Done | קיים ב-[python/vision/intersection_vision.py](../python/vision/intersection_vision.py) | - |
| זיהוי צפיפות תנועה | Done | `density_pct` מחושב ונשלח לשרת | - |
| זיהוי הולכי רגל | Done (By Scope) | לפי החלטת היקף הפרויקט, הולכי רגל מטופלים כנתיב תנועה רגיל במסגרת אותו pipeline | אין דרישה למודל הולכי רגל נפרד |
| ביטול אוטומטי של בקשות אם האובייקט נעלם | Missing | לא נמצא flow מלא שמבטל בקשת חירום/בקשת מעבר לפי disappearance event | נדרש מנגנון rule/state מפורש |

---

## 3. מנוע קבלת החלטות ובקרה

| דרישה מההצעה | סטטוס | מה קיים בפועל | מה עדיין חסר |
|---|---|---|---|
| בחירת קונפיגורציה תקפה מתוך קבוצת פאזות | Done | `Junction`, `Action`, `applyPhase`, `RLAgent::selectAction` | - |
| אילוצי בטיחות: זרימות מתנגשות לא ירוק יחד | Partial | conflict matrix חיצוני נטען מ-JSON ומאומת ב-[cpp/Junction.cpp](../cpp/Junction.cpp), וקונפיג פאזות נטען ב-runtime | עדיין אין phase plans ברמת תמרונים מלאה (פניות/הולכי רגל/שלבי ביניים) |
| אילוצי מינימום/מקסימום ירוק | Partial | `canSwitchPhase()` ו-`mustSwitchPhase()` + inter-green guard (`yellow+all-red`) ב-[cpp/Junction.cpp](../cpp/Junction.cpp) | עדיין אין state machine מלא עם מצבי צבע מפורשים לכל נתיב |
| מניעת starvation / fairness | Partial | waiting time tracking + reward penalties + thresholds | אין bounded-wait guarantee פורמלי או מנגנון fairness נפרד |
| RL / למידה מחיזוק | Partial | יש Q-learning טבלאי, Q-table persisted, reward function, simulation | לא מדובר עדיין במערכת למידה עשירה/רשתית/מרובת-צמתים |
| Rule-based fallback | Done | decision tree threshold-driven בתוך [cpp/RLAgent.cpp](../cpp/RLAgent.cpp) | אפשר להרחיב ולחדד חוקים |
| thresholds דינאמיים מהגדרות | Done | [cpp/ThresholdConfig.cpp](../cpp/ThresholdConfig.cpp), [cpp/traffic_thresholds.json](../cpp/traffic_thresholds.json) | - |
| קונפיגורציית בטיחות דינאמית | Done | [cpp/ConflictConfig.cpp](../cpp/ConflictConfig.cpp), [cpp/lane_conflicts.json](../cpp/lane_conflicts.json) | אפשר להוסיף phase plans מלאים |

---

## 4. תקשורת, שרת וארכיטקטורה

| דרישה מההצעה | סטטוס | מה קיים בפועל | מה עדיין חסר |
|---|---|---|---|
| שרת מרכזי/API | Done | FastAPI ב-[python/server/app.py](../python/server/app.py) | - |
| REST/HTTP להעברת מצב ופעולה | Done | `/state`, `/intersection/{id}`, `/packet`, `/action` | - |
| WebSocket לעדכונים רציפים | Missing | לא קיים מימוש WebSocket פעיל | נדרש אם רוצים לעמוד בהצעה המקורית |
| MQTT בין צמתים | Missing | לא קיים | נדרש רק אם בוחרים לממש לפי ההצעה המקורית |
| RTSP/H.264/H.265 pipeline מלא | Missing | אין מסלול end-to-end כזה במימוש הנוכחי | ניתן להישאר עם camera source מקומי באב-טיפוס |
| IPC פנימי בין רכיבים | Missing | לא זוהה מימוש נפרד | ייתכן שלא הכרחי לאב-טיפוס, אבל חסר מול ההצעה |

---

## 5. אבטחת מידע

| דרישה מההצעה | סטטוס | מה קיים בפועל | מה עדיין חסר |
|---|---|---|---|
| ערוץ מוצפן בין רכיבים | Missing | אין שכבת הצפנה ייעודית ברמת Transport | נדרש TLS/HTTPS אמיתי או מנגנון מוצפן בין בקרים |
| חתימה דיגיטלית להודעות | Partial | נוספה חתימת HMAC להודעות חירום ולהודעות שכנים | עדיין לא PKI/חתימה א-סימטרית מלאה |
| אימות אות GPS חירום | Partial | השרת מאמת `vehicle_id+lane_id+timestamp+signature` + anti-replay | נדרש ניהול מפתחות מלא ו-hardening ייצור |
| החלפת מפתחות מאובטחת | Missing | לא קיים | פיתוח עתידי |
| מניעת זיוף הודעות בין צמתים | Partial | neighbor summaries נחתמים ונבדקים ב-C++ | עדיין חסר auth/rotation/transport security מלא |

---

## 6. מסד נתונים ותצורה

| דרישה מההצעה | סטטוס | מה קיים בפועל | מה עדיין חסר |
|---|---|---|---|
| מסד נתונים לצמתים והגדרות | Partial | יש סכמות DB וקוד טעינת intersections | קיימת אי-אחידות בתיעוד בין SQL Server / MySQL |
| קריאת מספר מצלמות/נתיבים מה-DB | Done | קיים ב-[python/auto_launcher.py](../python/auto_launcher.py) | - |
| תצורה דינאמית ללא קומפילציה | Done | thresholds + conflicts נטענים מ-JSON בזמן ריצה | חסרה קונפיגורציה אחידה לטופולוגיה/פאזות |

---

## 7. סימולציה, בדיקות והערכת ביצועים

| דרישה מההצעה | סטטוס | מה קיים בפועל | מה עדיין חסר |
|---|---|---|---|
| הרצה בסימולציה | Done | `--simulate` עובד ב-[cpp/Simulation.cpp](../cpp/Simulation.cpp) | - |
| השוואה בין Baseline ל-RL | Done | מודפסת השוואה של avg_wait / max_queue / throughput | - |
| בדיקות חירום | Partial | קיימים תרחישי emergency בסיסיים בסימולציה | אין test suite רשמי ומובנה |
| בדיקות בטיחות | Partial | conflict validation קיים לוגית | אין regression tests / assertions אוטומטיים רשמיים |
| בדיקת מערכת מלאה | Partial | יש build והרצה, ויש נתוני סימולציה | אין מסמך KPI מסודר, אין benchmark מתועד |
| מסקנות ותוצאות סופיות | Missing | אין עדיין דו"ח ניסויי מסכם בפרויקט | נדרש לפני הגשה סופית |

### תוצאות KPI עדכניות (סימולציה, 5 seeds)

נמדדו שלושה מצבים:

- **Baseline**
- **RL(no-neighbor)** — RL ללא Neighbor Coordination
- **RL(neighbor)** — RL עם Neighbor Coordination

תוצאות:

- Baseline: `avg_wait=1.625`, `max_queue=95`, `throughput=1196`, `phase_switches=58`
- RL(no-neighbor): `avg_wait=1.633`, `max_queue=88`, `throughput=1199`, `phase_switches=56`
- RL(neighbor): `avg_wait=1.63833`, `max_queue=90`, `throughput=1197`, `phase_switches=57`

מסקנה כנה:

- Neighbor Coordination **ממומש פונקציונלית** ועובד מקצה לקצה.
- בוצעו סבבי טיוב ב-Reward וב-Neighbor state encoding.
- Neighbor Coordination נשאר **פונקציונלי**, אך כרגע עדיין ללא יתרון KPI עקבי מול RL בלי שכנים.
- שלב 1 (טיוב) בתהליך; נדרש המשך tuning עד להשגת שיפור מובהק במדדים המרכזיים.

### תוצאות Sweep אוטומטי לפרופילי Neighbor Tuning

המערכת מריצה כעת sweep אוטומטי על פרופילי neighbor חיצוניים:

- `neighbor_tuning_conservative.json`
- `neighbor_tuning_balanced.json`
- `neighbor_tuning_aggressive.json`

תוצאות אחרונות:

- RL(no-neighbor): `avg_wait=1.59933`, `max_queue=87`, `throughput=1198`, `phase_switches=57`
- RL(neighbor:conservative): `avg_wait=1.64533`, `max_queue=90`, `throughput=1197`, `phase_switches=56`
- RL(neighbor:balanced): `avg_wait=1.63467`, `max_queue=91`, `throughput=1197`, `phase_switches=57`
- RL(neighbor:aggressive): `avg_wait=1.63567`, `max_queue=89`, `throughput=1197`, `phase_switches=56`

Best profile לפי ציון משוקלל פנימי: **aggressive**.

הערכה: עדיין נדרש טיוב כדי ש-RL(neighbor) ינצח בעקביות את RL(no-neighbor).

---

## 8. פערים קריטיים שנותרו

להשלמת הפרויקט מול ההצעה המקורית, אלו הפערים הכי משמעותיים:

1. **אבטחת מידע ואימות אותות חירום**
   - נוספו חתימות HMAC + anti-replay לאותות חירום ולהודעות שכנים.
   - עדיין חסרים TLS מלא, ניהול/רוטציית מפתחות וחתימה א-סימטרית ברמת ייצור.

2. **מודל פאזות וטופולוגיה עשיר יותר**
   - נוסף קונפיג JSON לפאזות פר-צומת עם fallback לבנייה אוטומטית.
   - עדיין נדרש phase plan עשיר ברמת turn movements ושלבי מעבר.

3. **מצבי מעבר רמזור מלאים**
   - נוסף inter-green guard בטיחותי (yellow+all-red timing) בין פאזות.
   - עדיין חסר state machine מלא של צבעים מפורשים ברמת signal heads.

4. **דו"ח ניסויי והוכחת ביצועים**
   - יש סימולציה, אבל אין מסמך מסודר שמוכיח שיפור לפי תרחישים והמדדים שהוגדרו.

---

## 9. מה כבר אפשר לטעון ביושר

ניתן לטעון ביושר שכבר קיים:
- אב-טיפוס עובד של מערכת ניהול תנועה חכמה.
- תמיכה דינאמית במספר נתיבים.
- שילוב בין vision, server, ו-controller ב-C++.
- תמיכה בסיסית ברכבי חירום.
- RL טבלאי עם סימולציה והשוואה מול baseline.
- thresholds וקונפיגורציית בטיחות הניתנים לשינוי בזמן ריצה.

לא נכון עדיין לטעון שהמערכת משלימה במלואה:
- תיאום בין צמתים,
- אבטחת מידע מלאה,
- טיפול מלא בהולכי רגל,
- ומימוש מלא של כל דרישות ההצעה במערכת אחת סגורה ומבוססת ניסויים פורמליים.

---

## 10. המלצה להמשך

סדר עדיפויות מומלץ:

1. **להוסיף דו"ח תוצאות / KPI מסודר**
2. **להוסיף phase plan/topology config**
3. **להוסיף בדיקות אוטומטיות לבטיחות וחירום**
4. **להחליט אם נכנסים גם לאבטחת מידע בפועל או מגדירים זאת כעבודה עתידית**
5. **להרחיב את neighbor coordination ממנגנון בסיסי לסנכרון רשת מלא**

כך יהיה אפשר להציג פרויקט חזק, מדויק, ואמין גם טכנית וגם אקדמית.
