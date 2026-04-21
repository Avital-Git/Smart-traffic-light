"""db_intersections.py

חיבור למסד נתונים SQL Server בו נשמרים מיקומי צמתים.
אביטל חדד | מכללת בנות בת שבע
"""

import pyodbc
import os


# ══════════════════════════════════════════════════════
# הגדרות חיבור ל-SQL Server
# ══════════════════════════════════════════════════════

DB_SERVER = os.getenv("SQL_SERVER", r".\SQLEXPRESS")
DB_NAME = os.getenv("SQL_DATABASE", "smart_traffic")
# אם ריק — ישתמש ב-Windows Authentication (Trusted Connection)
DB_USER = os.getenv("SQL_USER", "")
DB_PASSWORD = os.getenv("SQL_PASSWORD", "")


def _build_connection_string() -> str:
    """בונה connection string ל-SQL Server."""
    driver = "{ODBC Driver 17 for SQL Server}"
    if DB_USER:
        return (
            f"DRIVER={driver};"
            f"SERVER={DB_SERVER};"
            f"DATABASE={DB_NAME};"
            f"UID={DB_USER};"
            f"PWD={DB_PASSWORD};"
        )
    else:
        return (
            f"DRIVER={driver};"
            f"SERVER={DB_SERVER};"
            f"DATABASE={DB_NAME};"
            f"Trusted_Connection=yes;"
        )


def get_connection():
    """מחזיר חיבור פתוח ל-SQL Server."""
    try:
        conn = pyodbc.connect(_build_connection_string())
        return conn
    except pyodbc.Error as e:
        print(f"[DB] שגיאת חיבור ל-SQL Server: {e}")
        print(f"[DB] Server: {DB_SERVER} | Database: {DB_NAME}")
        print("[DB] ודאי ש-SQL Server רץ וש-smart_traffic DB קיים.")
        raise


def fetch_intersections() -> list:
    """מחזירה רשימת צמתים כ-list of dicts."""
    conn = get_connection()
    try:
        cursor = conn.cursor()
        cursor.execute("SELECT * FROM dbo.intersections ORDER BY intersection_id")
        columns = [col[0] for col in cursor.description]
        rows = cursor.fetchall()
        return [dict(zip(columns, row)) for row in rows]
    finally:
        conn.close()


def fetch_neighbors(intersection_id: int) -> list:
    """מחזירה שכנים של צומת נתונה."""
    conn = get_connection()
    try:
        cursor = conn.cursor()
        cursor.execute(
            "SELECT n.*, i.name AS adjacent_name, "
            "i.latitude AS adjacent_lat, i.longitude AS adjacent_lon "
            "FROM dbo.intersection_neighbors n "
            "JOIN dbo.intersections i ON n.adjacent_intersection_id = i.intersection_id "
            "WHERE n.intersection_id = ?",
            (intersection_id,),
        )
        columns = [col[0] for col in cursor.description]
        rows = cursor.fetchall()
        return [dict(zip(columns, row)) for row in rows]
    finally:
        conn.close()


def insert_intersection(
    code: str, name: str, latitude: float, longitude: float,
    num_cameras: int = 4, city: str = None, region: str = None, description: str = None,
):
    """הכנסת צומת חדשה."""
    conn = get_connection()
    try:
        cursor = conn.cursor()
        cursor.execute(
            "INSERT INTO dbo.intersections "
            "(intersection_code, name, latitude, longitude, num_cameras, city, region, description) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (code, name, latitude, longitude, num_cameras, city, region, description),
        )
        conn.commit()
    finally:
        conn.close()


def fetch_intersection_by_id(intersection_id: int) -> dict:
    """מחזירה צומת בודדת לפי ID."""
    conn = get_connection()
    try:
        cursor = conn.cursor()
        cursor.execute(
            "SELECT * FROM dbo.intersections WHERE intersection_id = ?",
            (intersection_id,),
        )
        columns = [col[0] for col in cursor.description]
        row = cursor.fetchone()
        if row:
            return dict(zip(columns, row))
        return None
    finally:
        conn.close()


def update_intersection_cameras(intersection_id: int, num_cameras: int):
    """עדכון מספר מצלמות בצומת."""
    conn = get_connection()
    try:
        cursor = conn.cursor()
        cursor.execute(
            "UPDATE dbo.intersections SET num_cameras = ? WHERE intersection_id = ?",
            (num_cameras, intersection_id),
        )
        conn.commit()
    finally:
        conn.close()


if __name__ == "__main__":
    print("═" * 50)
    print("  בודק חיבור ל-SQL Server...")
    print("═" * 50)
    try:
        intersections = fetch_intersections()
        print(f"\n✅ נמצאו {len(intersections)} צמתים:\n")
        for row in intersections:
            print(
                f"  #{row['intersection_id']}: {row['intersection_code']} — "
                f"{row['name']} | {row['num_cameras']} מצלמות | "
                f"({row['latitude']}, {row['longitude']})"
            )
    except Exception as e:
        print(f"\n❌ שגיאה: {e}")
        print("\nוודאי:")
        print("  1. SQL Server רץ")
        print("  2. מסד הנתונים smart_traffic קיים (הריצי את database_schema_sqlserver.sql)")
        print(f"  3. שם השרת נכון: {DB_SERVER}")
