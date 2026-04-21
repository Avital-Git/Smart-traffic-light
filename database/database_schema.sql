-- database_schema.sql
-- מסד נתונים ל"מערכת ניהול תנועה חכמה" לפי הצעת הפרויקט
-- עדכון: תמיכה בנתיבים דינאמיים

CREATE DATABASE IF NOT EXISTS smart_traffic CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE smart_traffic;

-- טבלת צמתים עם מיקום גיאוגרפי ומספר מצלמות דינאמי
CREATE TABLE IF NOT EXISTS intersections (
  intersection_id INT AUTO_INCREMENT PRIMARY KEY,
  intersection_code VARCHAR(32) NOT NULL UNIQUE,
  name VARCHAR(128) NOT NULL,
  latitude DECIMAL(10,7) NOT NULL,
  longitude DECIMAL(10,7) NOT NULL,
  num_cameras INT DEFAULT 4,       -- מספר המצלמות בצומת (דינאמי!)
  city VARCHAR(64) DEFAULT NULL,
  region VARCHAR(64) DEFAULT NULL,
  description VARCHAR(255) DEFAULT NULL,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- דוגמה של נתוני מיקום צמתים עם מספר מצלמות שונה
INSERT INTO intersections (intersection_code, name, latitude, longitude, num_cameras, city, region, description) VALUES
  ('INT001', 'צומת ראשי מרכזי', 32.0853000, 34.7817680, 4, 'תל אביב', 'דרום', 'צומת ראשי בעיר, 4 כיוונים'),
  ('INT002', 'צומת בית ספר', 32.0773780, 34.7871110, 3, 'תל אביב', 'דרום', 'צומת משולש עם 3 כיוונים'),
  ('INT003', 'צומת תעשייה', 32.0674200, 34.7635300, 6, 'תל אביב', 'דרום', 'צומת גדולה עם 6 כיוונים!'),
  ('INT004', 'צומת שדה תעופה', 32.0000000, 34.8830000, 2, 'נתב"ג', 'מרכז', 'צומת בין שתי דרכים');

-- טבלת שכנים לצמתים (תקשורת בין צמתים סמוכים)
CREATE TABLE IF NOT EXISTS intersection_neighbors (
  neighbor_id INT AUTO_INCREMENT PRIMARY KEY,
  intersection_id INT NOT NULL,
  adjacent_intersection_id INT NOT NULL,
  direction_from VARCHAR(8) DEFAULT NULL,
  distance_m INT DEFAULT NULL,
  FOREIGN KEY (intersection_id) REFERENCES intersections(intersection_id) ON DELETE CASCADE,
  FOREIGN KEY (adjacent_intersection_id) REFERENCES intersections(intersection_id) ON DELETE CASCADE,
  UNIQUE KEY unique_neighbor (intersection_id, adjacent_intersection_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO intersection_neighbors (intersection_id, adjacent_intersection_id, direction_from, distance_m) VALUES
  (1, 2, 'S', 450),
  (2, 1, 'N', 450),
  (1, 3, 'W', 1800),
  (3, 1, 'E', 1800),
  (1, 4, 'E', 10500),
  (4, 1, 'W', 10500);
