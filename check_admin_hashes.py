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
    print("ADMIN USERS IN DATABASE:")
    print("=" * 70)
    
    cursor.execute("SELECT user_id, username, password_hash FROM dbo.admin_users ORDER BY user_id")
    rows = cursor.fetchall()
    
    if rows:
        for row in rows:
            user_id, username, pwd_hash = row
            print(f"ID: {user_id}  | Username: {username:15} | Hash: {pwd_hash[:30]}...")
    else:
        print("No admin users found!")
    
    print("=" * 70)
    
    cursor.close()
    conn.close()

except Exception as e:
    print(f"Error: {e}")
