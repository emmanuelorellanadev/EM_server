"""
database.py – SQLite data-access layer for the EM_server.

Stores sensor readings from any MQTT source (ESP8266, Raspberry Pi, etc.)
in a single normalised table so new sensor types can be added without
schema changes.
"""

import sqlite3
import json
from datetime import datetime
from zoneinfo import ZoneInfo

# Guatemala does not observe daylight saving time; it is always UTC-6.
_TZ_GUATEMALA = ZoneInfo("America/Guatemala")


def get_connection(db_path: str) -> sqlite3.Connection:
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


def init_db(db_path: str) -> None:
    """Create tables if they do not already exist."""
    with get_connection(db_path) as conn:
        conn.executescript("""
            CREATE TABLE IF NOT EXISTS readings (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                source      TEXT    NOT NULL,
                field       TEXT    NOT NULL,
                value       REAL    NOT NULL,
                unit        TEXT    NOT NULL DEFAULT '',
                recorded_at TEXT    NOT NULL
            );

            CREATE INDEX IF NOT EXISTS idx_readings_source
                ON readings (source);

            CREATE INDEX IF NOT EXISTS idx_readings_recorded_at
                ON readings (recorded_at);
        """)


def insert_reading(
    db_path: str,
    source: str,
    field: str,
    value: float,
    unit: str = "",
    recorded_at: datetime | None = None,
) -> None:
    if recorded_at is None:
        recorded_at = datetime.now(_TZ_GUATEMALA)
    ts = recorded_at.strftime("%Y-%m-%dT%H:%M:%S-06:00")
    with get_connection(db_path) as conn:
        conn.execute(
            "INSERT INTO readings (source, field, value, unit, recorded_at) "
            "VALUES (?, ?, ?, ?, ?)",
            (source, field, value, unit, ts),
        )


def insert_readings_from_payload(
    db_path: str, source: str, payload: dict
) -> None:
    """
    Parse a JSON MQTT payload and insert one row per sensor field.

    Expected payload shape (all fields optional):
    {
        "temperature":    { "value": 23.5, "unit": "°C"  },
        "humidity":       { "value": 60.1, "unit": "%"   },
        "soil_humidity":  { "value": 42.0, "unit": "%"   },
        "light":          { "value": 320,  "unit": "lux" },
        "pressure":       { "value": 1013, "unit": "hPa" }
    }

    Scalar numeric and boolean values are also accepted:
        { "temperature": 23.5, "humidity": 60.1 }
        { "percent": 42.3, "watering": false,
          "on_threshold_percent": 35, "relay_on_time_s": 1.0 }

    Boolean values are stored as 1.0 (True) or 0.0 (False).
    String values are silently skipped.
    """
    now = datetime.now(_TZ_GUATEMALA)
    for field, raw in payload.items():
        if isinstance(raw, dict):
            value = raw.get("value")
            unit = raw.get("unit", "")
        elif isinstance(raw, bool):
            # bool must be checked before int since bool is a subclass of int
            value = 1.0 if raw else 0.0
            unit = ""
        elif isinstance(raw, (int, float)):
            value = raw
            unit = ""
        else:
            continue  # skip non-numeric fields (e.g. strings)
        try:
            insert_reading(db_path, source, field, float(value), unit, now)
        except (TypeError, ValueError):
            pass


def get_latest_readings(db_path: str) -> list[dict]:
    """Return the most recent reading for each (source, field) pair."""
    sql = """
        SELECT source, field, value, unit, recorded_at
        FROM readings
        WHERE id IN (
            SELECT MAX(id)
            FROM readings
            GROUP BY source, field
        )
        ORDER BY source, field
    """
    with get_connection(db_path) as conn:
        rows = conn.execute(sql).fetchall()
    return [dict(r) for r in rows]


def get_readings_history(
    db_path: str,
    source: str | None = None,
    field: str | None = None,
    limit: int = 100,
    hours: int | None = None,
) -> list[dict]:
    """Return recent readings, optionally filtered by source, field, and/or time window.

    Args:
        db_path: Path to the SQLite database file.
        source:  Filter to a specific sensor source (e.g. ``"esp8266"``).
        field:   Filter to a specific field name (e.g. ``"temperature"``).
        limit:   Maximum number of rows to return (applied after other filters).
        hours:   When provided, only readings from the last *N* hours are
                 returned.  The cutoff timestamp is computed in Guatemala time
                 (UTC-6) to match the timezone used when data is stored.
    """
    from datetime import timedelta

    conditions: list[str] = []
    params: list = []
    if source:
        conditions.append("source = ?")
        params.append(source)
    if field:
        conditions.append("field = ?")
        params.append(field)
    if hours is not None:
        cutoff = (datetime.now(_TZ_GUATEMALA) - timedelta(hours=hours)).isoformat(
            timespec="seconds"
        )
        conditions.append("recorded_at >= ?")
        params.append(cutoff)
    where = ("WHERE " + " AND ".join(conditions)) if conditions else ""
    sql = f"""
        SELECT source, field, value, unit, recorded_at
        FROM readings
        {where}
        ORDER BY recorded_at DESC
        LIMIT ?
    """
    params.append(limit)
    with get_connection(db_path) as conn:
        rows = conn.execute(sql, params).fetchall()
    return [dict(r) for r in rows]


def get_sources(db_path: str) -> list[str]:
    """Return all distinct sensor sources seen so far."""
    with get_connection(db_path) as conn:
        rows = conn.execute(
            "SELECT DISTINCT source FROM readings ORDER BY source"
        ).fetchall()
    return [r["source"] for r in rows]
