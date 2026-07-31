"""SQLite data-access layer.

Stores readings in a single table:
- source: device name (esp8266, esp32_01, raspberrypi, ...)
- field: metric name
- value/unit/recorded_at: metric payload and timestamp
"""

import logging
import os
import sqlite3
from datetime import datetime
from typing import Optional
from zoneinfo import ZoneInfo

# Fixed timezone used for persisted timestamps.
_TZ_GUATEMALA = ZoneInfo("America/Guatemala")

logger = logging.getLogger("database")


def get_connection(db_path: str) -> sqlite3.Connection:
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


def init_db(db_path: str) -> None:
    """Create tables if they do not already exist."""
    db_dir = os.path.dirname(db_path)
    if db_dir:
        os.makedirs(db_dir, exist_ok=True)
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
    recorded_at: Optional[datetime] = None,
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
    logger.debug("Reading stored: source=%s field=%s value=%s", source, field, value)


def insert_readings_from_payload(
    db_path: str, source: str, payload: dict
) -> None:
    """
    Parse a JSON MQTT payload and insert one row per sensor field.

    Supported payload shapes:
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

    Booleans are stored as 1.0/0.0. Non-numeric strings are ignored.
    """
    now = datetime.now(_TZ_GUATEMALA)
    for field, raw in payload.items():
        if isinstance(raw, dict):
            value = raw.get("value")
            unit = raw.get("unit", "")
        elif isinstance(raw, bool):
            # bool must be checked before int (Python bool is int subclass)
            value = 1.0 if raw else 0.0
            unit = ""
        elif isinstance(raw, (int, float)):
            value = raw
            unit = ""
        else:
            continue
        try:
            insert_reading(db_path, source, field, float(value), unit, now)
        except (TypeError, ValueError):
            logger.warning(
                "Skipping field=%s with unparsable value=%r", field, raw
            )


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
    source: Optional[str] = None,
    field: Optional[str] = None,
    limit: int = 100,
) -> list[dict]:
    """Return recent readings, optionally filtered by source and/or field."""
    conditions: list[str] = []
    params: list = []
    if source:
        conditions.append("source = ?")
        params.append(source)
    if field:
        conditions.append("field = ?")
        params.append(field)
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


def get_field_history(
    db_path: str,
    source: str,
    field: str,
    since: Optional[datetime] = None,
    limit: Optional[int] = None,
) -> list[dict]:
    """Return time-ordered history for one source/field pair.

    Used by trend endpoints for source/field time series.

    Args:
        db_path: SQLite database path.
        source: Sensor source (for example: "esp8266").
        field:  Field name (for example: "soil_humidity").
        since:  Optional lower bound (inclusive) for ``recorded_at``.
        limit:  Optional cap of *most recent* rows to return.

    Returns:
        A list of dict rows sorted oldest -> newest.
    """
    conditions = ["source = ?", "field = ?"]
    params: list = [source, field]

    if since is not None:
        conditions.append("recorded_at >= ?")
        # Keep same timestamp format used by insert_reading().
        params.append(since.strftime("%Y-%m-%dT%H:%M:%S-06:00"))

    where = " AND ".join(conditions)

    if limit is not None:
        # Fetch newest rows, then reverse to chronological order.
        sql = f"""
            SELECT source, field, value, unit, recorded_at
            FROM readings
            WHERE {where}
            ORDER BY recorded_at DESC
            LIMIT ?
        """
        params_with_limit = [*params, limit]
        with get_connection(db_path) as conn:
            rows = conn.execute(sql, params_with_limit).fetchall()
        return [dict(r) for r in reversed(rows)]

    sql = f"""
        SELECT source, field, value, unit, recorded_at
        FROM readings
        WHERE {where}
        ORDER BY recorded_at ASC
    """
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


def get_last_valid_last_watering_epoch(db_path: str, source: str) -> Optional[float]:
    """Return the latest valid "ultimo riego" epoch for one source.

    Lookup strategy:
    1) last positive value from ``last_watering_at_epoch``.
    2) fallback from latest non-negative ``last_watered_sec`` + ``recorded_at``.
    """
    with get_connection(db_path) as conn:
        row_epoch = conn.execute(
            """
            SELECT value
            FROM readings
            WHERE source = ?
              AND field = 'last_watering_at_epoch'
              AND value > 0
            ORDER BY id DESC
            LIMIT 1
            """,
            (source,),
        ).fetchone()

        if row_epoch is not None:
            try:
                return float(row_epoch["value"])
            except (TypeError, ValueError):
                return None

        row_sec = conn.execute(
            """
            SELECT value, recorded_at
            FROM readings
            WHERE source = ?
              AND field = 'last_watered_sec'
              AND value >= 0
            ORDER BY id DESC
            LIMIT 1
            """,
            (source,),
        ).fetchone()

    if row_sec is None:
        return None

    try:
        sec = float(row_sec["value"])
        dt = datetime.fromisoformat(str(row_sec["recorded_at"]))
    except (TypeError, ValueError):
        return None

    epoch = dt.timestamp() - sec
    return epoch if epoch > 0 else None
