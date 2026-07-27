#!/usr/bin/env python3
"""Victron telemetry collector.

Polls one or more ESP32 devices (Huckleberry / Mervyns) over their existing
HTTP JSON APIs and writes normalized rows into a SQL Server Express database.
Pull model: all configuration lives here, no device firmware changes required.

Usage:
    python collector.py                 # loop forever at poll_interval_seconds
    python collector.py --once          # single pass (for Task Scheduler)
    python collector.py --config other.json
    python collector.py --dry-run       # poll + print, no database writes

Requires: pyodbc  (pip install -r requirements.txt) and the
"ODBC Driver 17 for SQL Server" (already present on this machine).
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import time
import urllib.request
from pathlib import Path

try:
    import pyodbc
except ImportError:
    print("pyodbc is not installed. Run: pip install -r requirements.txt", file=sys.stderr)
    raise

HERE = Path(__file__).resolve().parent


# --------------------------------------------------------------------------- #
# Config
# --------------------------------------------------------------------------- #
def load_config(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def connection_string(db: dict) -> str:
    parts = [
        f"Driver={{{db.get('driver', 'ODBC Driver 17 for SQL Server')}}}",
        f"Server={db['server']}",
        f"Database={db['database']}",
    ]
    if db.get("trusted_connection", True):
        parts.append("Trusted_Connection=yes")
    else:
        parts.append(f"Uid={db.get('username', '')}")
        parts.append(f"Pwd={db.get('password', '')}")
    parts.append("Encrypt=no")
    return ";".join(parts) + ";"


# --------------------------------------------------------------------------- #
# HTTP
# --------------------------------------------------------------------------- #
def fetch_json(url: str, timeout: float) -> dict:
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


# --------------------------------------------------------------------------- #
# Device adapters -> normalized dicts
#
# live  -> keys matching dbo.LiveSample columns (snake mapping below)
# daily -> list of dicts with the raw per-day fields (identical shape on both
#          devices): age, seq, yield, consumed, bmax, bmin, peak, imax, pvmax,
#          bulk, abs, float, errors[]
# --------------------------------------------------------------------------- #
def _num(v):
    return v if isinstance(v, (int, float)) else None


def adapt_huckleberry(host: str, timeout: float):
    st = fetch_json(f"{host}/api/state", timeout)
    sol = st.get("sol") or {}
    batt = st.get("batt") or {}
    th = st.get("th") or {}
    temps = batt.get("temps") or []
    meta = {
        "model": sol.get("model"),
        "serial": sol.get("serial"),
        "product_id": sol.get("productId"),
        "firmware": st.get("fw"),
    }
    live = {
        "BatteryV": _num(batt.get("v")) if batt.get("valid") else _num(sol.get("v")),
        "BatteryA": _num(batt.get("a")) if batt.get("valid") else _num(sol.get("a")),
        "BatteryW": _num(batt.get("w")),
        "Soc": _num(batt.get("soc")) if batt.get("valid") else None,
        "PvW": _num(sol.get("pv")),
        "PvV": _num(sol.get("pvV")),
        "LoadA": _num(sol.get("loadA")),
        "LoadV": _num(sol.get("loadV")),
        "LoadOn": bool(sol.get("loadOn")) if sol.get("loadOn") is not None else None,
        "YieldTodayKwh": _num(sol.get("yield")),
        "ChargeState": sol.get("state"),
        "DeviceState": None,
        "PeakTodayW": _num(sol.get("peakToday")),
        "MonthPeakW": _num(sol.get("monthPeak")),
        "InsideTempF": _num(th.get("inside")),
        "BattTempF": _num(temps[0]) if temps else None,
        "Rssi": _num(sol.get("rssi")),
    }
    hist = fetch_json(f"{host}/api/victron/history", timeout)
    # /api/state exposes no calendar date; fall back to the (co-located) PC date.
    return meta, live, (hist.get("days") or []), None


def adapt_mervyns(host: str, timeout: float):
    lv = fetch_json(f"{host}/api/live", timeout)
    bv, ba = _num(lv.get("batteryVoltage")), _num(lv.get("batteryCurrent"))
    meta = {
        "model": lv.get("model"),
        "serial": lv.get("serial"),
        "product_id": lv.get("productId"),
        "firmware": lv.get("fw"),
    }
    live = {
        "BatteryV": bv,
        "BatteryA": ba,
        "BatteryW": (bv * ba) if (bv is not None and ba is not None) else None,
        "Soc": None,  # no BMS on Mervyns
        "PvW": _num(lv.get("pvPowerWatts")),
        "PvV": _num(lv.get("pvVoltage")),
        "LoadA": _num(lv.get("loadCurrent")),
        "LoadV": _num(lv.get("loadVoltage")),
        "LoadOn": bool(lv.get("loadOn")) if lv.get("loadOn") is not None else None,
        "YieldTodayKwh": _num(lv.get("yieldTodayKwh")),
        "ChargeState": lv.get("chargeStage"),
        "DeviceState": _num(lv.get("deviceState")),
        "PeakTodayW": _num(lv.get("peakToday")),
        "MonthPeakW": _num(lv.get("monthPeak")),
        "InsideTempF": None,
        "BattTempF": None,
        "Rssi": _num(lv.get("rssi")),
    }
    hist = fetch_json(f"{host}/api/history", timeout)
    # Use the device's own clock (which is what its history "age" is relative to)
    # as the reference for day-0, so the row rolls over on the device's midnight,
    # not the PC's. Only trust it when recent, else fall back to the PC date.
    ref_date = None
    try:
        plocal = dt.datetime.strptime(lv["lastPacketLocal"], "%Y-%m-%d %H:%M:%S")
        if abs((dt.datetime.now() - plocal).total_seconds()) < 6 * 3600:
            ref_date = plocal.date()
    except (KeyError, ValueError, TypeError):
        pass
    return meta, live, (hist.get("days") or []), ref_date


ADAPTERS = {"huckleberry": adapt_huckleberry, "mervyns": adapt_mervyns}


# --------------------------------------------------------------------------- #
# Database
# --------------------------------------------------------------------------- #
def upsert_device(cur, key: str, name: str, host: str, meta: dict, now_utc) -> int:
    cur.execute(
        """
        MERGE dbo.Device AS t
        USING (SELECT ? AS DeviceKey) AS s ON t.DeviceKey = s.DeviceKey
        WHEN MATCHED THEN UPDATE SET
            Name = ?, Host = ?, Model = ?, Serial = ?, ProductId = ?,
            Firmware = ?, LastSeenUtc = ?
        WHEN NOT MATCHED THEN
            INSERT (DeviceKey, Name, Host, Model, Serial, ProductId, Firmware, LastSeenUtc)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?);
        """,
        key,
        name, host, meta.get("model"), meta.get("serial"), meta.get("product_id"),
        meta.get("firmware"), now_utc,
        key, name, host, meta.get("model"), meta.get("serial"), meta.get("product_id"),
        meta.get("firmware"), now_utc,
    )
    cur.execute("SELECT DeviceId FROM dbo.Device WHERE DeviceKey = ?", key)
    return cur.fetchone()[0]


LIVE_COLS = [
    "BatteryV", "BatteryA", "BatteryW", "Soc", "PvW", "PvV", "LoadA", "LoadV",
    "LoadOn", "YieldTodayKwh", "ChargeState", "DeviceState", "PeakTodayW",
    "MonthPeakW", "InsideTempF", "BattTempF", "Rssi",
]


def insert_live(cur, device_id: int, ts_utc, live: dict) -> None:
    cols = ", ".join(["DeviceId", "TsUtc"] + LIVE_COLS)
    marks = ", ".join(["?"] * (2 + len(LIVE_COLS)))
    # dedup on (DeviceId, TsUtc) so re-runs within the same second are no-ops
    cur.execute(
        f"""
        IF NOT EXISTS (SELECT 1 FROM dbo.LiveSample WHERE DeviceId = ? AND TsUtc = ?)
            INSERT INTO dbo.LiveSample ({cols}) VALUES ({marks});
        """,
        device_id, ts_utc,
        device_id, ts_utc, *[live.get(c) for c in LIVE_COLS],
    )


def upsert_daily(cur, device_id: int, today: dt.date, days: list) -> int:
    n = 0
    for d in days:
        age = d.get("age")
        if age is None:
            continue
        hist_date = today - dt.timedelta(days=int(age))
        errs = (d.get("errors") or [0, 0, 0, 0]) + [0, 0, 0, 0]
        cur.execute(
            """
            MERGE dbo.DailyHistory AS t
            USING (SELECT ? AS DeviceId, ? AS HistDate) AS s
                ON t.DeviceId = s.DeviceId AND t.HistDate = s.HistDate
            WHEN MATCHED THEN UPDATE SET
                Seq = ?, YieldKwh = ?, ConsumedKwh = ?, BattMinV = ?, BattMaxV = ?,
                PeakW = ?, ImaxA = ?, PvMaxV = ?, BulkMin = ?, AbsMin = ?, FloatMin = ?,
                Err0 = ?, Err1 = ?, Err2 = ?, Err3 = ?, UpdatedUtc = SYSUTCDATETIME()
            WHEN NOT MATCHED THEN
                INSERT (DeviceId, HistDate, Seq, YieldKwh, ConsumedKwh, BattMinV, BattMaxV,
                        PeakW, ImaxA, PvMaxV, BulkMin, AbsMin, FloatMin, Err0, Err1, Err2, Err3)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
            """,
            device_id, hist_date,
            d.get("seq"), d.get("yield"), d.get("consumed"), d.get("bmin"), d.get("bmax"),
            d.get("peak"), d.get("imax"), d.get("pvmax"), d.get("bulk"), d.get("abs"), d.get("float"),
            errs[0], errs[1], errs[2], errs[3],
            device_id, hist_date, d.get("seq"), d.get("yield"), d.get("consumed"),
            d.get("bmin"), d.get("bmax"), d.get("peak"), d.get("imax"), d.get("pvmax"),
            d.get("bulk"), d.get("abs"), d.get("float"), errs[0], errs[1], errs[2], errs[3],
        )
        n += 1
    return n


# --------------------------------------------------------------------------- #
# Poll cycle
# --------------------------------------------------------------------------- #
def log(msg: str) -> None:
    print(f"{dt.datetime.now():%Y-%m-%d %H:%M:%S}  {msg}", flush=True)


def poll_once(cfg: dict, conn, dry_run: bool) -> None:
    timeout = float(cfg.get("http_timeout_seconds", 8))
    now_utc = dt.datetime.now(dt.timezone.utc).replace(microsecond=0, tzinfo=None)
    pc_today = dt.date.today()  # fallback; devices are co-located with this PC

    for dev in cfg.get("devices", []):
        key = dev["key"]
        host = dev["host"]
        dtype = dev.get("type", key)
        name = dev.get("name", key)
        adapter = ADAPTERS.get(dtype)
        if adapter is None:
            log(f"[{key}] unknown device type '{dtype}', skipping")
            continue
        try:
            meta, live, days, ref_date = adapter(host, timeout)
        except Exception as exc:  # unreachable / bad JSON — skip this device
            log(f"[{key}] poll failed: {exc.__class__.__name__}: {exc}")
            continue
        # Day-0 reference: the device's own current date when it reports one,
        # so the daily row rolls over on the device's midnight, not the PC's.
        today = ref_date or pc_today

        if dry_run:
            log(f"[{key}] DRY  V={live['BatteryV']} A={live['BatteryA']} "
                f"PV={live['PvW']}W soc={live['Soc']} days={len(days)} ({meta.get('model')})")
            continue

        try:
            cur = conn.cursor()
            device_id = upsert_device(cur, key, name, host, meta, now_utc)
            insert_live(cur, device_id, now_utc, live)
            nd = upsert_daily(cur, device_id, today, days)
            conn.commit()
            log(f"[{key}] ok  live@{now_utc:%H:%M:%S}  daily={nd}  "
                f"V={live['BatteryV']} A={live['BatteryA']} PV={live['PvW']}W")
        except Exception as exc:
            conn.rollback()
            log(f"[{key}] db error: {exc.__class__.__name__}: {exc}")


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def main() -> int:
    ap = argparse.ArgumentParser(description="Victron -> SQL Server collector")
    ap.add_argument("--config", default=str(HERE / "config.json"))
    ap.add_argument("--once", action="store_true", help="single pass then exit")
    ap.add_argument("--dry-run", action="store_true", help="poll and print, no DB writes")
    args = ap.parse_args()

    cfg_path = Path(args.config)
    if not cfg_path.exists():
        print(f"Config not found: {cfg_path}\n"
              f"Copy config.example.json to config.json and edit it.", file=sys.stderr)
        return 2
    cfg = load_config(cfg_path)

    conn = None
    if not args.dry_run:
        conn = pyodbc.connect(connection_string(cfg["database"]), timeout=10)
        log(f"connected to {cfg['database']['server']} / {cfg['database']['database']}")

    interval = float(cfg.get("poll_interval_seconds", 60))
    try:
        while True:
            poll_once(cfg, conn, args.dry_run)
            if args.once:
                break
            time.sleep(interval)
    except KeyboardInterrupt:
        log("stopped")
    finally:
        if conn is not None:
            conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
