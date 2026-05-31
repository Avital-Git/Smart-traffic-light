import pyodbc

try:
    conn = pyodbc.connect(
        r"DRIVER={ODBC Driver 17 for SQL Server};"
        r"SERVER=.\SQLEXPRESS;"
        r"DATABASE=master;"
        r"Trusted_Connection=yes;"
    )
    cursor = conn.cursor()

    cursor.execute("SELECT name FROM sys.databases WHERE name = 'smart_traffic'")
    row = cursor.fetchone()

    if row:
        print("OK: מסד הנתונים smart_traffic קיים")
        cursor.execute("USE smart_traffic")
        cursor.execute("SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA='dbo'")
        tables = cursor.fetchall()
        if tables:
            print(f"טבלאות ({len(tables)}):")
            for t in tables:
                print(f"  - {t[0]}")
                cursor.execute(f"SELECT COUNT(*) FROM dbo.[{t[0]}]")
                print(f"    {cursor.fetchone()[0]} רשומות")
        else:
            print("אין טבלאות — יש להריץ database_schema_sqlserver.sql")
    else:
        print("MISSING: מסד הנתונים smart_traffic לא קיים")
        print("יש להריץ את הקובץ: database/database_schema_sqlserver.sql")

    conn.close()

except pyodbc.Error as e:
    print(f"שגיאת חיבור: {e}")
except Exception as e:
    print(f"שגיאה: {e}")
