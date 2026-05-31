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

IF OBJECT_ID(N'dbo.intersection_lanes', N'U') IS NOT NULL
BEGIN
    DROP TABLE dbo.intersection_lanes;
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

-- טבלת נתיבים/מצלמות לפי צומת וכיוון
CREATE TABLE dbo.intersection_lanes (
    lane_id         INT IDENTITY(1,1) PRIMARY KEY,
    intersection_id INT NOT NULL,
    camera_index    INT NOT NULL,
    direction       VARCHAR(2) NOT NULL CHECK (direction IN ('N','S','E','W','NE','NW','SE','SW')),
    description     NVARCHAR(100),
    FOREIGN KEY (intersection_id) REFERENCES dbo.intersections(intersection_id)
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

INSERT INTO dbo.intersection_lanes (intersection_id, camera_index, direction, description)
VALUES
    -- Intersection 1 (4 cameras): N, S, E, W
    (1, 0, 'N', N'Intersection 1 - North lane'),
    (1, 1, 'S', N'Intersection 1 - South lane'),
    (1, 2, 'E', N'Intersection 1 - East lane'),
    (1, 3, 'W', N'Intersection 1 - West lane'),

    -- Intersection 2 (3 cameras): N, S, E
    (2, 0, 'N', N'Intersection 2 - North lane'),
    (2, 1, 'S', N'Intersection 2 - South lane'),
    (2, 2, 'E', N'Intersection 2 - East lane'),

    -- Intersection 3 (6 cameras): N, S, E, W, NE, NW
    (3, 0, 'N', N'Intersection 3 - North lane'),
    (3, 1, 'S', N'Intersection 3 - South lane'),
    (3, 2, 'E', N'Intersection 3 - East lane'),
    (3, 3, 'W', N'Intersection 3 - West lane'),
    (3, 4, 'NE', N'Intersection 3 - North-East lane'),
    (3, 5, 'NW', N'Intersection 3 - North-West lane'),

    -- Intersection 4 (2 cameras): N, S
    (4, 0, 'N', N'Intersection 4 - North lane'),
    (4, 1, 'S', N'Intersection 4 - South lane');
GO

-- טבלת סכסוכי נתיבים - מזהה זוגות של נתיבים שלא יכולים להיות במצב ירוק בו זמנית
IF OBJECT_ID(N'dbo.lane_conflicts', N'U') IS NOT NULL
BEGIN
    DROP TABLE dbo.lane_conflicts;
END
GO

CREATE TABLE dbo.lane_conflicts (
    conflict_id     INT IDENTITY(1,1) PRIMARY KEY,
    intersection_id INT NOT NULL,
    lane_id_1       INT NOT NULL,
    lane_id_2       INT NOT NULL,
    conflict_type   NVARCHAR(50) NULL DEFAULT (N'crossing'), -- 'crossing', 'merging', 'diverging', etc.
    created_at      DATETIME2 NOT NULL CONSTRAINT DF_lane_conflicts_created_at DEFAULT (SYSUTCDATETIME()),
    CONSTRAINT FK_lane_conflicts_intersection
        FOREIGN KEY (intersection_id) REFERENCES dbo.intersections(intersection_id)
        ON DELETE CASCADE,
    CONSTRAINT FK_lane_conflicts_lane1
        FOREIGN KEY (lane_id_1) REFERENCES dbo.intersection_lanes(lane_id)
        ON DELETE CASCADE,
    CONSTRAINT FK_lane_conflicts_lane2
        FOREIGN KEY (lane_id_2) REFERENCES dbo.intersection_lanes(lane_id)
        ON DELETE CASCADE,
    CONSTRAINT UQ_unique_conflict UNIQUE (intersection_id, lane_id_1, lane_id_2),
    CONSTRAINT CK_different_lanes CHECK (lane_id_1 < lane_id_2)
);
GO

-- הוספת דוגמאות של סכסוכי נתיבים לצומת 1
-- סכסוך בין צפון לדרום (זוגות מתנוגדות)
INSERT INTO dbo.lane_conflicts (intersection_id, lane_id_1, lane_id_2, conflict_type)
VALUES
    (1, 1, 2, N'crossing'),  -- North-South conflict (lanes 1 and 2)
    (1, 3, 4, N'crossing');  -- East-West conflict (lanes 3 and 4)
GO

SELECT * FROM dbo.intersections ORDER BY intersection_id;
SELECT * FROM dbo.intersection_neighbors ORDER BY neighbor_id;
SELECT * FROM dbo.intersection_lanes ORDER BY lane_id;
SELECT * FROM dbo.lane_conflicts ORDER BY conflict_id;
GO
