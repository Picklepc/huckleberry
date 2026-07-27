/* ============================================================================
   VictronTelemetry — schema for the multi-device Victron collector.

   Idempotent and non-destructive: safe to run repeatedly. Creates a dedicated
   database and never touches any other database on the instance.

   Apply with:
     sqlcmd -S "localhost\SQLEXPRESS" -E -i schema.sql
   ============================================================================ */

IF DB_ID('VictronTelemetry') IS NULL
    CREATE DATABASE VictronTelemetry;
GO

USE VictronTelemetry;
GO

/* ---- Device: one row per physical charger/controller ---------------------- */
IF OBJECT_ID('dbo.Device') IS NULL
CREATE TABLE dbo.Device (
    DeviceId     INT           IDENTITY(1,1) PRIMARY KEY,
    DeviceKey    NVARCHAR(64)  NOT NULL UNIQUE,   -- stable slug from config, e.g. 'huckleberry'
    Name         NVARCHAR(128) NULL,
    Host         NVARCHAR(160) NULL,
    Model        NVARCHAR(128) NULL,
    Serial       NVARCHAR(64)  NULL,
    ProductId    INT           NULL,
    Firmware     NVARCHAR(32)  NULL,
    FirstSeenUtc DATETIME2(0)  NOT NULL DEFAULT SYSUTCDATETIME(),
    LastSeenUtc  DATETIME2(0)  NULL
);
GO

/* ---- LiveSample: high-resolution poll-time snapshots ---------------------- */
IF OBJECT_ID('dbo.LiveSample') IS NULL
CREATE TABLE dbo.LiveSample (
    SampleId      BIGINT       IDENTITY(1,1) PRIMARY KEY,
    DeviceId      INT          NOT NULL
        REFERENCES dbo.Device(DeviceId),
    TsUtc         DATETIME2(0) NOT NULL,          -- collector poll time (UTC)
    BatteryV      FLOAT        NULL,
    BatteryA      FLOAT        NULL,              -- +charge / -discharge
    BatteryW      FLOAT        NULL,
    Soc           FLOAT        NULL,              -- real SOC where a BMS exists (Huckleberry); NULL otherwise
    PvW           FLOAT        NULL,
    PvV           FLOAT        NULL,
    LoadA         FLOAT        NULL,
    LoadV         FLOAT        NULL,
    LoadOn        BIT          NULL,
    YieldTodayKwh FLOAT        NULL,
    ChargeState   NVARCHAR(24) NULL,
    DeviceState   INT          NULL,
    PeakTodayW    FLOAT        NULL,
    MonthPeakW    FLOAT        NULL,
    InsideTempF   FLOAT        NULL,              -- Huckleberry (ambient from EcoWorthy sensor)
    BattTempF     FLOAT        NULL,
    Rssi          INT          NULL,
    CONSTRAINT UX_LiveSample_Device_Ts UNIQUE (DeviceId, TsUtc)
);
GO

IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = 'IX_LiveSample_Device_Ts')
    CREATE INDEX IX_LiveSample_Device_Ts ON dbo.LiveSample (DeviceId, TsUtc DESC);
GO

/* ---- DailyHistory: one row per device per calendar day -------------------- */
/* Idempotent by (DeviceId, HistDate). The collector upserts every poll so the
   current day keeps refreshing and a device reconnecting after time away
   backfills up to its last 31 stored days. This table is the permanent record
   that grows past the charger's 31-day window. */
IF OBJECT_ID('dbo.DailyHistory') IS NULL
CREATE TABLE dbo.DailyHistory (
    DeviceId    INT          NOT NULL
        REFERENCES dbo.Device(DeviceId),
    HistDate    DATE         NOT NULL,
    Seq         INT          NULL,               -- Victron record sequence number
    YieldKwh    FLOAT        NULL,
    ConsumedKwh FLOAT        NULL,
    BattMinV    FLOAT        NULL,
    BattMaxV    FLOAT        NULL,
    PeakW       FLOAT        NULL,
    ImaxA       FLOAT        NULL,               -- peak charge current
    PvMaxV      FLOAT        NULL,
    BulkMin     INT          NULL,               -- minutes in each charge stage
    AbsMin      INT          NULL,
    FloatMin    INT          NULL,
    Err0        INT          NULL,
    Err1        INT          NULL,
    Err2        INT          NULL,
    Err3        INT          NULL,
    UpdatedUtc  DATETIME2(0) NOT NULL DEFAULT SYSUTCDATETIME(),
    CONSTRAINT PK_DailyHistory PRIMARY KEY (DeviceId, HistDate)
);
GO

/* ---- Convenience view: newest live sample per device ---------------------- */
IF OBJECT_ID('dbo.vLatestLive') IS NOT NULL
    DROP VIEW dbo.vLatestLive;
GO
CREATE VIEW dbo.vLatestLive AS
SELECT d.DeviceKey, d.Name, d.Model, d.Serial, ls.*
FROM dbo.Device d
CROSS APPLY (
    SELECT TOP (1) *
    FROM dbo.LiveSample s
    WHERE s.DeviceId = d.DeviceId
    ORDER BY s.TsUtc DESC
) ls;
GO
