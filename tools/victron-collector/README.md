# Victron Telemetry Collector

Polls one or more ESP32 solar devices (Huckleberry, Mervyns, …) over their
existing HTTP JSON APIs and stores charger-owned history in a **SQL Server
Express** database — so you keep **more than the charger's 31-day window** and
can query/report across **multiple devices**.

This is a **pull** collector: it runs on the PC, all configuration lives in
`config.json`, and **no device firmware changes are required**. A device just
needs to be reachable on the LAN (i.e. the rig is on the home network).

```
ESP32 device(s)  ──HTTP/JSON──►  collector.py (this PC)  ──►  SQL Server Express (SQLEXPRESS)
```

## What gets stored

| Table              | Grain                    | Notes |
|--------------------|--------------------------|-------|
| `dbo.Device`       | one row per device       | key, name, model, serial, firmware, last-seen |
| `dbo.DailyHistory` | one row per device·day   | Victron daily record: yield, consumed, peak, charge-stage minutes, min/max V. **Upserted** — grows forever, backfills up to 31 days on reconnect. |
| `dbo.IntradaySample` | one row per device·30 min | Charger-stored PV, voltage, current, and temperature trends. Current/finalized days refresh each collector pass; available backfill refreshes every six hours. |

Live API values are not inserted into SQL. If the trailer is away from the home
network for days or weeks, the next successful pass backfills the records still
stored by the charger instead of treating sparse poll times as a live timeline.

## One-time setup

1. **Install the Python dependency** (Python 3.9+; ODBC Driver 17 for SQL
   Server is already present on this machine):
   ```bash
   pip install -r requirements.txt
   ```

2. **Create the database** (idempotent, non-destructive — only creates
   `VictronTelemetry`, never touches other databases):
   ```bash
   sqlcmd -S "localhost\SQLEXPRESS" -E -i schema.sql
   ```

3. **Configure** — copy the example and edit the device list:
   ```bash
   copy config.example.json config.json
   ```
   `config.json` is git-ignored. Each device needs `key` (stable slug),
   `type` (`huckleberry` or `mervyns`), and `host` (base URL). Add more devices
   by appending to the array — the DB is multi-device by design.

## Running

```bash
python collector.py            # loop forever at poll_interval_seconds
python collector.py --once     # single pass (use from Task Scheduler)
python collector.py --dry-run  # poll + print, no DB writes (handy first check)
```

### Run it unattended

Register a Scheduled Task that launches the **continuous loop** at logon (runs
whether or not this window is open). From an **elevated** PowerShell:

```powershell
.\register-task.ps1
```

That creates task `VictronCollector`. Remove it with:
```powershell
Unregister-ScheduledTask -TaskName VictronCollector -Confirm:$false
```

Task registration requires a genuinely elevated Administrator PowerShell. If
Windows reports `Access is denied`, no task was created; reopen PowerShell with
**Run as administrator** and rerun `register-task.ps1`.

Prefer a periodic trigger instead of a long-running loop? Point a Basic Task at
`pythonw.exe collector.py --once` on a 30-minute schedule.

## Example queries

```sql
-- 31+ day yield trend for one device
SELECT HistDate, YieldKwh, PeakW, BulkMin, AbsMin, FloatMin
FROM VictronTelemetry.dbo.DailyHistory h
JOIN VictronTelemetry.dbo.Device d ON d.DeviceId = h.DeviceId
WHERE d.DeviceKey = 'huckleberry'
ORDER BY HistDate DESC;

-- charger-stored half-hour solar and battery trends
SELECT SampleTimeUtc, PvPowerW, PvVoltageV, BatteryVoltageV, ChargeCurrentA
FROM VictronTelemetry.dbo.IntradaySample s
JOIN VictronTelemetry.dbo.Device d ON d.DeviceId = s.DeviceId
WHERE d.DeviceKey = 'huckleberry'
  AND SampleTimeUtc >= DATEADD(day, -7, SYSUTCDATETIME())
ORDER BY SampleTimeUtc;
```

## Retention: >31-day data is never lost

The charger only exposes its **last 31 days**. The collector's whole point is to
outlive that window, so the upsert is deliberately **additive**:

- Each poll runs one `MERGE` per day the device reports, scoped to a single
  `(DeviceId, HistDate)`. There is **no `WHEN NOT MATCHED BY SOURCE ... DELETE`**
  clause, so a `MERGE` can only *insert or update* the specific date it targets.
- Dates older than the device's 31-day window are simply not in any poll, so
  nothing ever touches them — they remain in `dbo.DailyHistory` indefinitely.
- There is no retention job, trigger, or cleanup anywhere in the schema or
  collector. Rows only leave the table if *you* delete them.

(Verified: planting a row dated a year before the device's window and running a
full collector pass leaves it untouched.)

## Notes / assumptions

- **`DailyHistory.HistDate`** is derived as *(this PC's local date) − age*, which
  assumes the devices are co-located with this PC (same time zone). They are.
- **Day rollover:** every poll upserts *all* days the device reports, keyed by
  absolute date, using the **device's own current date** (Mervyns' last-packet
  clock) as day-0 when available, else the PC date. So when midnight passes, the
  row that was "today" is overwritten with the charger's *finalized* totals (now
  the device's age-1 record) and a fresh "today" row begins — automatically, on
  the next poll, with no duplicates.
- Unreachable device → that device is logged and skipped; other devices and the
  next cycle are unaffected.

Older installations may still contain a legacy `dbo.LiveSample` table. The
current collector never writes to it, and the schema removes its convenience
view without deleting old rows. Drop the table manually only if those existing
rows are no longer wanted.

## Collection is not a database backup

The collector and its Scheduled Task run entirely on this PC. They pull the
ESP32 HTTP APIs and write stored charger rows to SQL Server; the ESP32 never
connects directly to SQL Server and holds no database credentials.
`register-task.ps1` does not create a SQL Server `.bak` file or a disaster-
recovery schedule, which would be a separate PC-side job.
