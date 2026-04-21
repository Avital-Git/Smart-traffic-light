# הוראות מפורטות - התחלה מאפס

## שלב 1: התקנה בסיסית

### 1.1 סביבת Python

בתיקייה הראשית של הפרויקט:

```bash
# אם עדיין אין לך סביבה וירטואלית
python -m venv .venv

# הפעלת הסביבה
# ב-Windows:
.\.venv\Scripts\activate
# ב-Linux/Mac:
source .venv/bin/activate

# התקנת כל התלויות
pip install -r requirements.txt
```

### 1.2 מסד נתונים MySQL

#### אפשרות 1: שימוש בבנייה גרפית (MySQL Workbench)

1. פתח את MySQL Workbench
2. התחבר לשרת MySQL שלך
3. קובץ → פתח SQL Script
4. בחר `database_schema.sql` מהפרויקט
5. לחץ Execute (Ctrl+Shift+Enter)

#### אפשרות 2: שימוש בשורת הפקודה

```bash
# כניסה ל-MySQL
mysql -u root -p

# ואז בתוך mysql:
source c:\Users\User\Desktop\smart traffic project\database_schema.sql

# או בעמדה אחת:
mysql -u root -p < "c:\Users\User\Desktop\smart traffic project\database_schema.sql"
```

#### אפשרות 3: שימוש בקובץ Python

```bash
python db_intersections.py
```

### 1.3 בדיקת מסד הנתונים

```bash
python
>>> from db_intersections import fetch_intersections
>>> intersections = fetch_intersections()
>>> for row in intersections:
...     print(row)
```

## שלב 2: הפעלת השרת

בתיקייה הראשית, טרמינל חדש:

```bash
python -m uvicorn server.app:app --reload --host 127.0.0.1 --port 8000
```

תראי:
```
Uvicorn running on http://127.0.0.1:8000
```

## שלב 3: הפעלת מפעיל צמתים (AUTO-LAUNCHER)

בטרמינל שלישי, טרמינל חדש:

```bash
python auto_launcher.py
```

זה **יגלה בעצמו** כמה מצלמות בכל צומת מהמסד נתונים ויפעיל את המערכת!

דוגמה לפלט:
```
════════════════════════════════════════════════════════
        מערכת ניהול תנועה חכמה — מפעיל אוטומטי
════════════════════════════════════════════════════════
[AUTO-LAUNCHER] מצאתי 4 צמתים במסד הנתונים
[AUTO-LAUNCHER] צומת 1: 4 מצלמות
[AUTO-LAUNCHER] צומת 2: 3 מצלמות
[AUTO-LAUNCHER] צומת 3: 6 מצלמות
[AUTO-LAUNCHER] צומת 4: 2 מצלמות
[AUTO-LAUNCHER] יצרתי analyzer לצומת 1 עם 4 נתיבים
...
```

## שלב 4 (אופציונלי): בדיקת API

בטרמינל רביעי:

```bash
curl http://127.0.0.1:8000/health
```

או לבדיקה מלאה של הערכת מצב:

```bash
curl -X POST http://127.0.0.1:8000/state \
  -H "Content-Type: application/json" \
  -d '{
    "intersection_id": 1,
    "num_lanes": 4,
    "timestamp": 1234567890.0,
    "lanes": [
      {"lane_id": 0, "vehicle_count": 5, "pedestrian_count": 2, "density_pct": 65.0, "waiting_time_sec": 12.0},
      {"lane_id": 1, "vehicle_count": 3, "pedestrian_count": 0, "density_pct": 15.0, "waiting_time_sec": 5.0},
      {"lane_id": 2, "vehicle_count": 8, "pedestrian_count": 1, "density_pct": 75.0, "waiting_time_sec": 20.0},
      {"lane_id": 3, "vehicle_count": 2, "pedestrian_count": 0, "density_pct": 10.0, "waiting_time_sec": 2.0}
    ]
  }'
```

## שלב 5: ממשק React (אפציונלי לעכשיו)

```bash
cd website
npm install
npm start
```

## דינאמיות המערכת

**זה ההדבר החשוב!**

המערכת אוטומטית:
- קוראת מ-MySQL כמה מצלמות בכל צומת (`num_cameras`)
- יוצרת `IntersectionAnalyzer` עם מספר הנתיבים המתאים
- מייצרת `RL vector` בגודל דינאמי: `(4 * num_lanes + 1)`
- שולחת את המצב לשרת

**דוגמה:**
- צומת 1 עם 4 מצלמות → `IntersectionAnalyzer(num_lanes=4)` → RL vector בגודל 17
- צומת 2 עם 3 מצלמות → `IntersectionAnalyzer(num_lanes=3)` → RL vector בגודל 13
- צומת 3 עם 6 מצלמות → `IntersectionAnalyzer(num_lanes=6)` → RL vector בגודל 25

## טרובלשוטינג

### שגיאה: "Cannot connect to MySQL"

```bash
# בדוק שה-MySQL service רץ:
# Windows:
Get-Service MySQL80

# Linux:
sudo service mysql status
```

### שגיאה: "Cannot open video capture device"

זה בסדר - המערכת מצפה למצלמה אמיתית. בדוגמה משתמשים ב-`camera_source=0`.

### שגיאה: Port 8000 already in use

```bash
# השתמש בפורט אחר:
python -m uvicorn server.app:app --host 127.0.0.1 --port 8001
```

## סיכום

```
┌─────────────────────────────────────────┐
│ 1. MySQL → database_schema.sql           │
│ 2. Server → uvicorn server.app:app       │
│ 3. Auto-Launcher → python auto_launcher.py
│ 4. Frontend → npm start (website/)       │
└─────────────────────────────────────────┘
```

**כל צומת מקבלת את מספר הנתיבים שלה בעצמה מהמסד נתונים!**
