# Smart Traffic Project

מערכת ניהול תנועה חכמה - פרויקט גמר

## 🎯 תמיכה דינאמית בנתיבים - המערכת מגלה את עצמה!

**עדכון גדול:** המערכת כעת **דינאמית לחלוטין** - היא קוראת מ-SQL Server כמה מצלמות יש בכל צומת ופועלת בהתאם!

- צומת עם 2 מצלמות? ✓ עובד
- צומת עם 4 מצלמות? ✓ עובד  
- צומת עם 10 מצלמות? ✓ עובד

**כל צומת מקבלת את הגדרתה מהמסד נתונים!**

---

## 📁 מבנה הפרויקט

```
smart-traffic-project/
├── vision/
│   ├── intersection_vision.py      # עיבוד וידאו + YOLO זיהוי
│   └── __init__.py
├── cpp/server/
│   ├── TrafficServer.cpp           # שרת C++ פעיל
│   └── Database.cpp
├── controller/
│   ├── rl_agent.h/.cpp             # C++ - RL החלטות
│   ├── main.cpp
│   └── CMakeLists.txt
├── website/
│   ├── src/                        # React ממשק לקוח
│   └── package.json
├── database_schema.sql             # SQL סכימה עם num_cameras
├── db_intersections.py             # קישור ל-SQL Server
├── auto_launcher.py                # מפעיל אוטומטי דינאמי
├── requirements.txt                # תלויות Python
├── GETTING_STARTED.md              # הוראות מלאות
└── _backup/                        # עותקי גיבוי
```

---

## 🚀 התחלה מהירה (3 צעדים)

### 1️⃣ סביבה + תלויות

```bash
python -m venv .venv
.\.venv\Scripts\activate  # Windows

pip install -r requirements.txt
```

### 2️⃣ מסד נתונים

```bash
# בחר אחת:
sqlcmd -S localhost -E -i database/database_schema_sqlserver.sql
# או
python db_intersections.py
```

### 3️⃣ הפעלה

**טרמינל 1:**
```bash
cpp\build\Release\traffic_server.exe 8000
```

**טרמינל 2:**
```bash
python python/auto_launcher.py
```

**טרמינל 3 (אופציונלי - בקר RL מלא):**
```bash
cpp\build\Release\smart_traffic_controller.exe --server 127.0.0.1 8000
```

✅ **סיים!** המערכת קוראת את SQL Server ופועלת עם מספר דינאמי של נתיבים!

---

## 🎬 כיצד זה עובד

```
1. SQL Server מכיל את num_cameras לכל צומת
   ↓
2. auto_launcher.py קורא מהמסד
   ↓
3. יוצר IntersectionAnalyzer(num_lanes=num_cameras)
   ↓
4. כל צומת שלחה state לשרת
   ↓
5. שרת מחליט על פעולה (אור ירוק לנתיב X)
```

---

## 📊 דינאמיות בעצמה

### SQL Server טבלה

```sql
CREATE TABLE intersections (
  intersection_id INT,
  name VARCHAR(128),
  num_cameras INT,  -- <-- דינאמי!
  ...
);

-- דוגמה:
('INT001', 'צומת ראשי', 4)      -- 4 נתיבים
('INT002', 'צומת בית ספר', 3)   -- 3 נתיבים
('INT003', 'צומת גדולה', 6)     -- 6 נתיבים!
```

### RL Vector גודל משתנה

```
num_cameras=3 → RL vector בגודל 13
num_cameras=4 → RL vector בגודל 17
num_cameras=6 → RL vector בגודל 25

נוסחה: (4 * num_cameras) + 1
```

---

## 🛠️ טכנולוגיות

- **Python**: YOLO, OpenCV, SQL Server (Vision/Feeder)
- **C++**: traffic_server + RL controller
- **React**: ממשק משתמש (בבנייה)
- **SQL Server**: מסד נתונים דינאמי

---

## 📚 תיעוד

- [GETTING_STARTED.md](GETTING_STARTED.md) - הוראות מלאות
- [cpp/README.md](cpp/README.md) - C++ server + controller
- [client/README.md](client/README.md) - React

---

## 💡 שאלה: SQL - היכן להשאיר?

**תשובה:** `database_schema.sql` נשאר בפרויקט כחלק מהתשתית.

**אפשרויות שימוש:**

1. **ישירה מ-SQL Server Management Studio (SSMS)** (קל ביותר)
   - פתח את הקובץ → Execute

2. **שורת פקודה**
   - `sqlcmd -S localhost -E -i database/database_schema_sqlserver.sql`

3. **דרך Python**
   - `python db_intersections.py`

**כל שלוש דרכים יוצרות את המסד וטבלות המצלמות הדינאמיות!**

---

## ✅ בדיקה

```bash
# בדיקת שרת
curl http://127.0.0.1:8000/health

# בדיקת SQL Server
python db_intersections.py
```

---

**מערכת ניהול תנועה חכמה עם תמיכה דינאמית מלאה לכל מספר מצלמות!**
