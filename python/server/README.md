# Smart Traffic Server

שרת FastAPI בסיסי עבור פרויקט ניהול תנועה חכמה.

## מה יש כאן

- `POST /state` - מקבל `IntersectionState` ומחזיר פעולה מומלצת.
- `GET /health` - בדיקת תקינות.
- `GET /intersection/{intersection_id}` - מחזיר את המצב האחרון שנשלח בצומת.
- `GET /intersection/{intersection_id}/action` - מחזיר את הפעולה המומלצת מהמצב האחרון.

## הרצה

מהתיקייה הראשית של הפרויקט:

```bash
python -m uvicorn server.app:app --reload --host 127.0.0.1 --port 8000
```

## דוגמא לפנייה ל-API

```bash
curl -X POST http://127.0.0.1:8000/state \
  -H "Content-Type: application/json" \
  -d '{
    "intersection_id": 1,
    "timestamp": 1234567890.0,
    "lanes": {
      "N": {"direction": "N", "vehicle_count": 5, "pedestrian_count": 2, "density_pct": 65.0, "waiting_time_sec": 12.0},
      "S": {"direction": "S", "vehicle_count": 3, "pedestrian_count": 0, "density_pct": 15.0, "waiting_time_sec": 5.0},
      "E": {"direction": "E", "vehicle_count": 8, "pedestrian_count": 1, "density_pct": 75.0, "waiting_time_sec": 20.0},
      "W": {"direction": "W", "vehicle_count": 2, "pedestrian_count": 0, "density_pct": 10.0, "waiting_time_sec": 2.0}
    },
    "emergency_signal": {"active": false}
  }'
```
