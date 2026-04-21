-- database_schema_sqlserver.sql
-- SQL Server version for Smart Traffic Project

IF DB_ID(N'smart_traffic') IS NULL
BEGIN
    CREATE DATABASE smart_traffic;
END
GO

USE smart_traffic;
GO

IF OBJECT_ID(N'dbo.intersection_neighbors', N'U') IS NOT NULL
BEGIN
    DROP TABLE dbo.intersection_neighbors;
END
GO

IF OBJECT_ID(N'dbo.intersections', N'U') IS NOT NULL
BEGIN
    DROP TABLE dbo.intersections;
END
GO

-- טבלת צמתים עם מיקום גיאוגרפי ומספר מצלמות דינאמי
CREATE TABLE dbo.intersections (
    intersection_id INT IDENTITY(1,1) PRIMARY KEY,
    intersection_code NVARCHAR(32) NOT NULL UNIQUE,
    name NVARCHAR(128) NOT NULL,
    latitude DECIMAL(10,7) NOT NULL,
    longitude DECIMAL(10,7) NOT NULL,
    num_cameras INT NOT NULL CONSTRAINT DF_intersections_num_cameras DEFAULT(4),
    city NVARCHAR(64) NULL,
    region NVARCHAR(64) NULL,
    description NVARCHAR(255) NULL,
    created_at DATETIME2 NOT NULL CONSTRAINT DF_intersections_created_at DEFAULT (SYSUTCDATETIME())
);
GO

INSERT INTO dbo.intersections (intersection_code, name, latitude, longitude, num_cameras, city, region, description)
VALUES
    (N'INT001', N'צומת ראשי מרכזי', 32.0853000, 34.7817680, 4, N'תל אביב', N'דרום', N'צומת ראשי בעיר, 4 כיוונים'),
    (N'INT002', N'צומת בית ספר', 32.0773780, 34.7871110, 3, N'תל אביב', N'דרום', N'צומת משולש עם 3 כיוונים'),
    (N'INT003', N'צומת תעשייה', 32.0674200, 34.7635300, 6, N'תל אביב', N'דרום', N'צומת גדולה עם 6 כיוונים!'),
    (N'INT004', N'צומת שדה תעופה', 32.0000000, 34.8830000, 2, N'נתב"ג', N'מרכז', N'צומת בין שתי דרכים');
GO

-- טבלת שכנים לצמתים
CREATE TABLE dbo.intersection_neighbors (
    neighbor_id INT IDENTITY(1,1) PRIMARY KEY,
    intersection_id INT NOT NULL,
    adjacent_intersection_id INT NOT NULL,
    direction_from NVARCHAR(8) NULL,
    distance_m INT NULL,
    CONSTRAINT FK_neighbors_intersection
        FOREIGN KEY (intersection_id) REFERENCES dbo.intersections(intersection_id)
        ON DELETE CASCADE,
    CONSTRAINT FK_neighbors_adjacent
        FOREIGN KEY (adjacent_intersection_id) REFERENCES dbo.intersections(intersection_id)
        ON DELETE NO ACTION,
    CONSTRAINT UQ_unique_neighbor UNIQUE (intersection_id, adjacent_intersection_id)
);
GO

INSERT INTO dbo.intersection_neighbors (intersection_id, adjacent_intersection_id, direction_from, distance_m)
VALUES
    (1, 2, N'S', 450),
    (2, 1, N'N', 450),
    (1, 3, N'W', 1800),
    (3, 1, N'E', 1800),
    (1, 4, N'E', 10500),
    (4, 1, N'W', 10500);
GO

SELECT * FROM dbo.intersections ORDER BY intersection_id;
SELECT * FROM dbo.intersection_neighbors ORDER BY neighbor_id;
GO
