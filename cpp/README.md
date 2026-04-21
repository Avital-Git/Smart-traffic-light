# Smart Traffic Controller

ב-folder זה יש שלד ראשוני של מנוע החלטות C++.

## מה קיים

- `rl_agent.h` ו-`rl_agent.cpp` — מבנים ראשוניים ל-`IntersectionState`, `EmergencySignal` ו-`MultiAgentController`.
- `main.cpp` — דוגמא להרצה של בחירת פעולה על פי מצב סימולציה.
- `CMakeLists.txt` — קובץ בנייה ל-CMake.

## איך להריץ

```bash
cd controller
mkdir build
cd build
cmake ..
cmake --build .
./smart_traffic_controller
```

## המשך עבודה

- לשלב קבלת מצב מ-Python דרך קבצי JSON, socket או API.
- להוסיף אתחול מדיניות מבוסס חוקים לפני ביצוע למידת חיזוק.
- להרחיב את `Action` לתרחישים של זמן מעבר (intergreen) ומניעת התנגשויות.
