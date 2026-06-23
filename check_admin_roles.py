import pyodbc

try:
    conn = pyodbc.connect(
        r"DRIVER={ODBC Driver 17 for SQL Server};"
        r"SERVER=.\SQLEXPRESS;"
        r"DATABASE=smart_traffic;"
        r"Trusted_Connection=yes;"
    )
    cursor = conn.cursor()
    
    print("=" * 70)
    print("ADMIN USERS AND THEIR ROLES:")
    print("=" * 70)
    
    cursor.execute("SELECT user_id, username, role FROM dbo.admin_users ORDER BY user_id")
    rows = cursor.fetchall()
    
    if rows:
        for row in rows:
            user_id, username, role = row
            role_display = role if role else "(NULL - will default to regular_admin)"
            print(f"ID: {user_id}  | Username: {username:15} | Role: {role_display}")
    else:
        print("No admin users found!")
    
    print("=" * 70)
    
    cursor.close()
    conn.close()

except Exception as e:
    print(f"Error: {e}")
