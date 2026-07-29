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

/* ---- IntradaySample: charger-owned 30-minute stored trends ---------------- */
IF OBJECT_ID('dbo.IntradaySample') IS NULL
CREATE TABLE dbo.IntradaySample (
    DeviceId             INT          NOT NULL
        REFERENCES dbo.Device(DeviceId),
    SampleTimeUtc         DATETIME2(0) NOT NULL,
    OutputCurrentA        FLOAT        NULL,
    PvVoltageV            FLOAT        NULL,
    PvPowerW              FLOAT        NULL,
    BatteryTempC          FLOAT        NULL,
    BatteryVoltageV       FLOAT        NULL,
    ChargeCurrentA        FLOAT        NULL,
    SourceIntervalSeconds INT          NOT NULL DEFAULT 1800,
    UpdatedUtc            DATETIME2(0) NOT NULL DEFAULT SYSUTCDATETIME(),
    CONSTRAINT PK_IntradaySample PRIMARY KEY (DeviceId, SampleTimeUtc)
);
GO

IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = 'IX_IntradaySample_Device_Time')
    CREATE INDEX IX_IntradaySample_Device_Time
        ON dbo.IntradaySample (DeviceId, SampleTimeUtc DESC);
GO

/* v0.5.3 stores only charger-owned history. Remove the legacy convenience
   view if an older schema created it, but leave old live rows/table untouched. */
IF OBJECT_ID('dbo.vLatestLive') IS NOT NULL
    DROP VIEW dbo.vLatestLive;
GO
