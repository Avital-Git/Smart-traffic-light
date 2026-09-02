# Smart Traffic Controller

מערכת חכמה לניהול תנועה בצמתים, המשלבת למידה חיזוקית, זיהוי תנועה באמצעות Computer Vision, ניהול צמתים בזמן אמת ותקשורת בין צמתים.

## תמצית

הפרויקט מתמקד בשלושה עקרונות מרכזיים:

- ניהול תנועה חכם בצמתים באמצעות בקרה דינמית
- הקדמה לרכבי חירום כדי להפחית עיכובים ולהבטיח תגובה מהירה
- גל ירוק בין צמתים סמוכים כדי לשפר את זרימת התנועה ולהקטין עצירות מיותרות

המערכת כוללת גם מנגנון של קונפיגורציה דינמית, תמיכה במידע מהמסד, ויכולות ניתוח KPI למעקב אחרי ביצועים.

## תכונות עיקריות

- שליטה בזמן אמת של רמזורים בצמתים
- בחירה אוטומטית של פאזה באמצעות Reinforcement Learning
- זיהוי כלי רכב ונתיבים באמצעות OpenCV + YOLO
- טיפול ברכבי חירום עם עדיפות מיידית
- תיאום בין צמתים סמוכים ליצירת גל ירוק
- זיהוי התנגשויות בין נתיבים והגבלת פאזה לא בטוחה
- ממשק React לצפייה בסטטוס ובמדדים

## קדימות לרכבי חירום

המערכת תומכת במנגנון של עדיפות לרכבי חירום על פני תנועה רגילה. כאשר מת detected רכב חירום, המערכת:

- מזהה את הגעתו דרך הזרם/הנתונים של הצומת
- מעלה את עדיפות העדכון של הפאזה הרלוונטית
- מפעילה מעבר למצב המאפשר תנועת רכבי החירום ללא עיכוב מיותר
- מונעת חסימה של נתיב החירום על ידי בקרת התנגשויות ופלטפורמת קבלת החלטות בזמן אמת

מטרה: הפחתת זמן התגובה, שיפור בטיחות, והבטחת מעבר מהיר ונגיש יותר לשירותי חירום.

## גל ירוק בין צמתים

המערכת כוללת גם מנגנון תיאום בין צמתים סמוכים, שנועד לאפשר גל ירוק לאורך מסלול תחבורה רציף. עיקרון העבודה:

- כל צומת משתף מידע על עומס, מצב זרימה ומצב שכנים
- המערכת מקצה פאזה תואמת לזרימת התנועה בשכנים
- כאשר יש רצף תנועה בכיוון מסוים, הצמתים מסונכרנים כך שהאור ירוק נמשך לאורך הדרך
- הדבר מפחית עצירות מיותרות, משפר את קצב התנועה ושומר על רציפות תנועה טובה יותר

זהו רכיב חשוב לצמצום המתנה בצמתים סמוכים ולשיפור ביצועי הרשת העירונית בכללותה.

## ארכיטקטורה

```text
React Dashboard
      │
      ▼
Python API / Services
      │
      ├── Vision (OpenCV + YOLO)
      ├── Metrics / KPI
      ├── Emergency handling
      └── Neighbor coordination
      │
      ▼
C++ Traffic Controller
      │
      ├── RL decision engine
      ├── Phase scheduling
      ├── Conflict logic
      └── Green-wave coordination
      │
      ▼
SQL / JSON runtime configuration
```

## מבנה הפרויקט

```text
smart-traffic-project/
├── cpp/                          # לוגיקת הבקרה ב-C++
├── python/                      # שרת Python, זיהוי, KPI, ניהול תנועה
├── client/                      # ממשק React
├── database/                    # סכמות SQL ונתוני צמתים
├── docs/                        # תיעוד נוסף
├── reports/                     # דוחות KPI וסטטיסטיקות
├── scripts/                     # סקריפטים מסייעים
├── .env.example                 # משתני סביבה לדוגמה
├── .gitignore                   # קבצי התעלמות מ-Git
├── GETTING_STARTED.md           # מדריך התקנה והפעלה
├── PROJECT_COMPLETION_REPORT.md # דוח מצב הפרויקט
├── README.md                    # המדריך הראשי
├── yolov8n.pt                   # מודל YOLO מראש
└── ...
```

## טכנולוגיות

- Python: FastAPI, OpenCV, Ultralytics YOLO, pyodbc, websockets
- C++: מנוע קבלת החלטות, סינכרון צמתים, לוגיקת פאזה
- React: ממשק משתמש וציונים בזמן אמת
- SQL Server: נתוני צמתים ותצורות דינמיות
- CMake: בניית קוד C++

## דרישות מערכת

- Python 3.10+
- Node.js 18+
- Visual Studio 2022 עם C++ build tools
- SQL Server
- Windows 10/11 מומלץ

## הפעלה מהירה

### 1) יצירת סביבה פייתונית

```bash
python -m venv .venv
.\.venv\Scripts\activate
pip install -r python/requirements.txt
```

### 2) התקנת תלויות frontend

```bash
cd client
npm install
```

### 3) בניית ה-C++

```bash
cd cpp
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### 4) הפעלה מלאה

```bash
python python/run_e2e.py --with-client
```

### 5) הפעלה ידנית

```bash
cpp\build\Release\traffic_server.exe 8000
python python/auto_launcher.py
```

ואם רוצים גם את ממשק המשתמש:

```bash
cd client
npm start
```

## בדיקות

בדיקת בריאות השרת:

```bash
curl http://127.0.0.1:8000/health
```

הרצת בדיקות מערכת:

```bash
python python/system_test_suite.py
```

## תיעוד נוסף

- [GETTING_STARTED.md](GETTING_STARTED.md) — מדריך התקנה מפורט
- [PROJECT_COMPLETION_REPORT.md](PROJECT_COMPLETION_REPORT.md) — סיכום פרויקט וביצועים
- [cpp/README.md](cpp/README.md) — פרטים על מנוע הצומת והבקרה
- [client/README.md](client/README.md) — שימוש בממשק המשתמש

## סטטוס

הפרויקט מוכן כבסיס למערכת בקרה חכמה לצמתים עירוניים, עם תמיכה במצבי אמת, תיאום בין צמתים, הקדמת רכבי חירום, ויכולת הרחבה עתידית.

> לפני העלאה ל-GitHub, כדאי להוסיף רישיון מתאים, למשל MIT או Apache 2.0.
