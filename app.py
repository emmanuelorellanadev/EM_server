"""
app.py – Flask web dashboard for EM_server.

Displays live and historical sensor readings received from the MQTT network
and allows sending remote commands to IoT devices (e.g. start watering on
the ESP8266) via a simple REST API that publishes to the MQTT broker.

Run:
    python app.py [--config config.json]
"""

import argparse
import json
import logging
import os
from datetime import datetime, timedelta
from typing import Optional
from zoneinfo import ZoneInfo
from flask import Flask, jsonify, render_template, request

import paho.mqtt.publish as mqtt_publish
import database as database_module

from database import (
    get_latest_readings,
    get_last_valid_last_watering_epoch,
    get_readings_history,
    get_sources,
    init_db,
)

# Configure module-level logger so all log calls in this file are formatted
# consistently and visible in the console / system journal.
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger("app")

app = Flask(__name__)

# ---------------------------------------------------------------------------
# Template helpers
# ---------------------------------------------------------------------------

_FIELD_ICONS = {
    "temperature":          "🌡️",
    "humidity":             "💧",
    "soil_humidity":        "🌱",
    "online":               "📡",
    "light":                "☀️",
    "pressure":             "🌀",
    "watering":             "🚿",
    "on_threshold_percent": "🎯",
    "relay_on_time_s":      "⏱️",
}
_FIELD_LABELS = {
    "temperature":          "Temperatura",
    "humidity":             "Humedad Ambiental",
    "soil_humidity":        "Humedad de Suelo",
    "online":               "Conectado MQTT",
    "light":                "Iluminación",
    "pressure":             "Presión Atmosférica",
    "watering":             "Riego Activo",
    "on_threshold_percent": "Umbral de Activación (%)",
    "relay_on_time_s":      "Duración de Riego (s)",
}

# Fields that represent boolean on/off state (stored as 1.0 / 0.0)
BOOLEAN_FIELDS = {"watering", "online"}
HIDDEN_FIELDS = {"last_watered_sec", "last_watering_at_epoch", "relay_on_time_s"}
DISPLAY_TZ = ZoneInfo("America/Guatemala")
NO_LAST_WATERING_TEXT = "Ultimo riego: sin registro"
NO_RELAY_DURATION_TEXT = "Duracion de riego: sin dato"
NO_THRESHOLD_TEXT = "Umbral de activacion: sin dato"
LAST_WATERED_SEC_FIELD = "last_watered_sec"
LAST_WATERING_EPOCH_FIELD = "last_watering_at_epoch"
RELAY_ON_TIME_FIELD = "relay_on_time_s"
ON_THRESHOLD_FIELD = "on_threshold_percent"
ESP_PANEL_SOURCES = {"esp8266", "esp32_01"}
DEFAULT_TREND_SOURCE = "esp8266"
SOURCE_TREND_FIELDS: dict[str, tuple[str, ...]] = {
    "esp8266": ("soil_humidity", "on_threshold_percent"),
    "esp32_01": ("soil_humidity", "on_threshold_percent", "temperature", "humidity"),
    "raspberrypi": ("temperature", "humidity", "pressure"),
}
DEFAULT_TREND_FIELDS = SOURCE_TREND_FIELDS[DEFAULT_TREND_SOURCE]

# Trend ranges requested for the irrigation chart selector.
#
# Keys are API values used by the frontend dropdown.
# Labels are display-friendly text shown/returned to the client.
# bucket_seconds keeps the chart readable on long ranges.
TREND_RANGE_CONFIG: dict[str, dict] = {
    "1h": {
        "label": "Ultima hora",
        "delta": timedelta(hours=1),
        "bucket_seconds": 0,
    },
    "1d": {
        "label": "Ultimo dia",
        "delta": timedelta(days=1),
        "bucket_seconds": 30 * 60,
    },
    "1w": {
        "label": "Ultima semana",
        "delta": timedelta(weeks=1),
        "bucket_seconds": 60 * 60,
    },
    "1m": {
        "label": "Ultimo mes",
        "delta": timedelta(days=30),
        "bucket_seconds": 6 * 60 * 60,
    },
    "1y": {
        "label": "Ultimo anio",
        "delta": timedelta(days=365),
        "bucket_seconds": 24 * 60 * 60,
    },
}


@app.template_global()
def field_icon(field: str) -> str:
    return _FIELD_ICONS.get(field, "📊")


@app.template_global()
def field_label(field: str) -> str:
    return _FIELD_LABELS.get(field, field.replace("_", " ").title())


@app.template_global()
def is_boolean_field(field: str) -> bool:
    return field in BOOLEAN_FIELDS


@app.template_global()
def should_render_field(field: str) -> bool:
    return field not in HIDDEN_FIELDS


@app.template_global()
def is_esp_panel_source(source: str) -> bool:
    return source in ESP_PANEL_SOURCES


def format_last_watering(epoch_value) -> str:
    """Build a user-friendly label for the latest watering timestamp."""
    try:
        epoch = float(epoch_value)
    except (TypeError, ValueError):
        return NO_LAST_WATERING_TEXT

    if epoch <= 0:
        return NO_LAST_WATERING_TEXT

    dt = datetime.fromtimestamp(epoch, DISPLAY_TZ)
    return f"Ultimo riego: {dt.strftime('%d/%m/%Y %H:%M:%S')}"


def _recorded_at_to_epoch(recorded_at_value) -> Optional[float]:
    """Parse an ISO timestamp from DB and return epoch seconds."""
    if not recorded_at_value:
        return None

    try:
        dt = datetime.fromisoformat(str(recorded_at_value))
    except ValueError:
        return None

    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=DISPLAY_TZ)
    return dt.timestamp()


def format_relay_duration(seconds_value) -> str:
    """Build a user-friendly label for relay-on duration in seconds."""
    try:
        seconds = float(seconds_value)
    except (TypeError, ValueError):
        return NO_RELAY_DURATION_TEXT

    if seconds < 0:
        return NO_RELAY_DURATION_TEXT

    if seconds.is_integer():
        return f"Duracion de riego: {int(seconds)} s"

    return f"Duracion de riego: {seconds:.1f} s"


def format_threshold(percent_value) -> str:
    """Build a user-friendly label for watering threshold percentage."""
    try:
        percent = float(percent_value)
    except (TypeError, ValueError):
        return NO_THRESHOLD_TEXT

    if percent < 0:
        return NO_THRESHOLD_TEXT

    if percent.is_integer():
        return f"Umbral de activacion: {int(percent)} %"

    return f"Umbral de activacion: {percent:.1f} %"


def build_latest_by_source(latest_readings: list[dict]) -> dict[str, dict[str, dict]]:
    """Index latest readings as ``source -> field -> row``.

    This avoids scanning the same list multiple times from templates and keeps
    card rendering deterministic.
    """
    by_source: dict[str, dict[str, dict]] = {}
    for reading in latest_readings:
        source = reading.get("source")
        field = reading.get("field")
        if not source or not field:
            continue
        by_source.setdefault(source, {})[field] = reading
    return by_source


def _reading_as_float(source_fields: dict[str, dict], field: str) -> Optional[float]:
    """Safely read a numeric field from ``source_fields``."""
    reading = source_fields.get(field)
    if reading is None:
        return None
    try:
        return float(reading.get("value"))
    except (TypeError, ValueError):
        return None


def build_last_watering_epoch_by_source(
    latest_by_source: dict[str, dict[str, dict]],
) -> dict[str, float]:
    """Resolve one epoch timestamp per source for "ultimo riego".

    Resolution order:
    1) latest ``last_watering_at_epoch`` if valid.
    2) derive from latest ``last_watered_sec`` + its ``recorded_at``.
    3) DB fallback when ESP sends sentinel ``last_watered_sec = -1``.
    """
    by_source: dict[str, float] = {}

    for source, source_fields in latest_by_source.items():
        epoch = _reading_as_float(source_fields, LAST_WATERING_EPOCH_FIELD)
        if epoch is not None and epoch > 0:
            by_source[source] = epoch
            continue

        sec_value = _reading_as_float(source_fields, LAST_WATERED_SEC_FIELD)
        if sec_value is not None and sec_value >= 0:
            sec_row = source_fields.get(LAST_WATERED_SEC_FIELD, {})
            recorded_epoch = _recorded_at_to_epoch(sec_row.get("recorded_at"))
            if recorded_epoch is not None:
                computed = recorded_epoch - sec_value
                if computed > 0:
                    by_source[source] = computed
                    continue

        fallback_epoch = get_last_valid_last_watering_epoch(_db_path, source)
        if fallback_epoch is not None and fallback_epoch > 0:
            by_source[source] = fallback_epoch

    return by_source


def build_last_watering_by_source(
    latest_by_source: dict[str, dict[str, dict]],
) -> tuple[dict[str, str], dict[str, float]]:
    """Build both label and raw epoch maps for card rendering."""
    epoch_by_source = build_last_watering_epoch_by_source(latest_by_source)
    label_by_source: dict[str, str] = {}
    for source, epoch in epoch_by_source.items():
        label_by_source[source] = format_last_watering(epoch)
    return label_by_source, epoch_by_source


def build_relay_duration_by_source(
    latest_by_source: dict[str, dict[str, dict]],
) -> dict[str, str]:
    """Collect one relay-duration label per source from latest readings."""
    by_source: dict[str, str] = {}
    for source, source_fields in latest_by_source.items():
        by_source[source] = format_relay_duration(
            _reading_as_float(source_fields, RELAY_ON_TIME_FIELD)
        )
    return by_source


def build_threshold_by_source(
    latest_by_source: dict[str, dict[str, dict]],
) -> dict[str, str]:
    """Collect one threshold label per source from latest readings."""
    by_source: dict[str, str] = {}
    for source, source_fields in latest_by_source.items():
        by_source[source] = format_threshold(
            _reading_as_float(source_fields, ON_THRESHOLD_FIELD)
        )
    return by_source


def _decorate_history_rows(readings: list[dict]) -> list[dict]:
    """Add UI-friendly display fields for history rows.

    Why this exists:
    - ``last_watered_sec = -1`` is a sentinel from ESP8266 after reboot.
    - End users need to see the latest known "ultimo riego" date instead of
      repeated ``-1`` values.

    This function keeps raw DB data intact and only enriches rows for the
    history template.
    """
    # We only need DB fallback for sources that contain the sentinel field.
    sources_needing_fallback = {
        row.get("source")
        for row in readings
        if row.get("field") == LAST_WATERED_SEC_FIELD
    }
    fallback_epoch_by_source: dict[str, float] = {}
    for source in sources_needing_fallback:
        if not source:
            continue
        epoch = get_last_valid_last_watering_epoch(_db_path, source)
        if epoch is not None and epoch > 0:
            fallback_epoch_by_source[source] = epoch

    decorated: list[dict] = []
    for row in readings:
        # Copy row so we never mutate the original DB payload in-place.
        current = dict(row)
        current["display_field"] = row.get("field", "")
        current["display_value"] = None
        current["display_unit"] = row.get("unit", "")
        current["display_is_numeric"] = True

        field = row.get("field")
        source = row.get("source")

        if field == LAST_WATERING_EPOCH_FIELD:
            current["display_field"] = "ultimo_riego"
            current["display_value"] = format_last_watering(row.get("value"))
            current["display_unit"] = ""
            current["display_is_numeric"] = False

        elif field == LAST_WATERED_SEC_FIELD:
            try:
                sec_value = float(row.get("value"))
            except (TypeError, ValueError):
                sec_value = None

            if sec_value is not None and sec_value < 0:
                current["display_field"] = "ultimo_riego"
                fallback_epoch = fallback_epoch_by_source.get(source)
                if fallback_epoch is not None:
                    current["display_value"] = format_last_watering(fallback_epoch)
                else:
                    current["display_value"] = NO_LAST_WATERING_TEXT
                current["display_unit"] = ""
                current["display_is_numeric"] = False

        decorated.append(current)

    return decorated


def _to_datetime(value: str) -> Optional[datetime]:
    """Parse ISO timestamp text into datetime.

    Returns None for invalid values so callers can skip bad rows safely.
    """
    try:
        return datetime.fromisoformat(value)
    except (TypeError, ValueError):
        return None


def _bucket_trend_points(rows: list[dict], bucket_seconds: int) -> list[dict]:
    """Convert DB rows into chart points, with optional averaging buckets.

    Args:
        rows: list of readings sorted by time (oldest -> newest)
        bucket_seconds: 0 disables bucketing; any positive value enables it

    Returns:
        List of points compatible with Chart.js: [{"x": iso, "y": number}, ...]
    """
    if bucket_seconds <= 0:
        points: list[dict] = []
        for row in rows:
            try:
                points.append({"x": row["recorded_at"], "y": float(row["value"])})
            except (KeyError, TypeError, ValueError):
                continue
        return points

    # Bucket shape: {bucket_epoch: {"sum": float, "count": int}}
    buckets: dict[int, dict[str, float]] = {}
    for row in rows:
        dt = _to_datetime(str(row.get("recorded_at", "")))
        if dt is None:
            continue
        try:
            value = float(row.get("value"))
        except (TypeError, ValueError):
            continue

        bucket_epoch = int(dt.timestamp() // bucket_seconds) * bucket_seconds
        if bucket_epoch not in buckets:
            buckets[bucket_epoch] = {"sum": 0.0, "count": 0.0}

        buckets[bucket_epoch]["sum"] += value
        buckets[bucket_epoch]["count"] += 1.0

    points = []
    for bucket_epoch in sorted(buckets.keys()):
        total = buckets[bucket_epoch]["sum"]
        count = buckets[bucket_epoch]["count"]
        if count <= 0:
            continue

        dt = datetime.fromtimestamp(bucket_epoch, tz=DISPLAY_TZ)
        points.append({
            "x": dt.isoformat(timespec="seconds"),
            "y": total / count,
        })
    return points


def _build_trend_response(source: str, range_key: str) -> dict:
    """Build trend payload for one source and one selected range.

    This function is intentionally straightforward so it is easy to teach and
    maintain. Supported fields are resolved by source in SOURCE_TREND_FIELDS.
    """
    cfg = TREND_RANGE_CONFIG[range_key]
    now = datetime.now(DISPLAY_TZ)
    since = now - cfg["delta"]
    trend_fields = SOURCE_TREND_FIELDS.get(source, DEFAULT_TREND_FIELDS)

    datasets: dict[str, list[dict]] = {}
    for field in trend_fields:
        # Backward-compatible access:
        # if get_field_history() exists in database.py we use it.
        # If not (for example, stale deployment files), we gracefully
        # fallback to get_readings_history() and filter in Python.
        if hasattr(database_module, "get_field_history"):
            rows = database_module.get_field_history(
                _db_path,
                source=source,
                field=field,
                since=since,
                limit=None,
            )
        else:
            # Fallback path for older database.py versions.
            # We fetch enough rows, then apply time filtering and ordering.
            fallback_rows = get_readings_history(
                _db_path,
                source=source,
                field=field,
                limit=20000,
            )
            rows = list(reversed(fallback_rows))
            filtered_rows: list[dict] = []
            for row in rows:
                dt = _to_datetime(str(row.get("recorded_at", "")))
                if dt is not None and dt >= since:
                    filtered_rows.append(row)
            rows = filtered_rows

        datasets[field] = _bucket_trend_points(rows, cfg["bucket_seconds"])

    return {
        "source": source,
        "range": range_key,
        "range_label": cfg["label"],
        "fields": list(trend_fields),
        "datasets": datasets,
    }

_config: dict = {}
_db_path: str = "em_server.db"


# ---------------------------------------------------------------------------
# HTML routes
# ---------------------------------------------------------------------------


@app.route("/")
def index():
    latest = get_latest_readings(_db_path)
    sources = get_sources(_db_path)
    latest_by_source = build_latest_by_source(latest)
    last_watering_by_source, last_watering_epoch_by_source = build_last_watering_by_source(
        latest_by_source
    )
    relay_duration_by_source = build_relay_duration_by_source(latest_by_source)
    threshold_by_source = build_threshold_by_source(latest_by_source)

    return render_template(
        "index.html",
        latest=latest,
        latest_by_source=latest_by_source,
        sources=sources,
        last_watering_by_source=last_watering_by_source,
        last_watering_epoch_by_source=last_watering_epoch_by_source,
        relay_duration_by_source=relay_duration_by_source,
        threshold_by_source=threshold_by_source,
    )


@app.route("/history")
def history():
    source = request.args.get("source")
    field = request.args.get("field")
    limit = int(request.args.get("limit", 100))
    readings = get_readings_history(_db_path, source=source, field=field, limit=limit)
    readings = _decorate_history_rows(readings)
    sources = get_sources(_db_path)
    return render_template(
        "history.html",
        readings=readings,
        sources=sources,
        selected_source=source,
        selected_field=field,
    )


# ---------------------------------------------------------------------------
# JSON API routes
# ---------------------------------------------------------------------------


@app.route("/api/latest")
def api_latest():
    return jsonify(get_latest_readings(_db_path))


@app.route("/api/history")
def api_history():
    source = request.args.get("source")
    field = request.args.get("field")
    limit = int(request.args.get("limit", 100))
    return jsonify(get_readings_history(_db_path, source=source, field=field, limit=limit))


@app.route("/api/sources")
def api_sources():
    return jsonify(get_sources(_db_path))


@app.route("/api/trend")
def api_trend():
    """Return irrigation trend data for a selected time range.

    Query params:
        source: device source (default: esp8266)
        range:  one of 1h|1d|1w|1m|1y

    Response:
        {
          "source": "esp8266",
          "range": "1d",
          "range_label": "Ultimo dia",
          "fields": ["soil_humidity", "on_threshold_percent"],
          "datasets": {
            "soil_humidity": [{"x": "...", "y": 45.2}],
            "on_threshold_percent": [{"x": "...", "y": 25.0}]
          }
        }
    """
    source = (request.args.get("source") or DEFAULT_TREND_SOURCE).strip()
    range_key = (request.args.get("range") or "1h").strip().lower()

    if range_key not in TREND_RANGE_CONFIG:
        return jsonify({
            "error": "Invalid range",
            "valid_ranges": list(TREND_RANGE_CONFIG.keys()),
        }), 400

    return jsonify(_build_trend_response(source=source, range_key=range_key))


# ---------------------------------------------------------------------------
# Command API routes — publish MQTT commands to IoT devices
# ---------------------------------------------------------------------------


def _mqtt_publish_command(mqtt_cfg: dict, topic: str, payload: dict) -> None:
    """Publish a single MQTT command message and disconnect immediately.

    Uses paho.mqtt.publish.single() which establishes a temporary connection,
    publishes the message with QoS 1 (at-least-once delivery), and closes the
    connection. This is ideal for infrequent, one-shot commands from the web
    dashboard without keeping a persistent MQTT connection inside the Flask
    process.

    Args:
        mqtt_cfg: The ``mqtt`` block from config.json.
        topic:    MQTT topic to publish to (e.g. "commands/esp8266").
        payload:  Dict that will be serialised to JSON and sent as the message body.

    Raises:
        Exception: Any network or broker error propagated from paho.
    """
    auth = None
    if mqtt_cfg.get("username"):
        auth = {
            "username": mqtt_cfg["username"],
            "password": mqtt_cfg.get("password", ""),
        }

    mqtt_publish.single(
        topic,
        payload=json.dumps(payload),
        qos=1,               # at-least-once delivery
        hostname=mqtt_cfg["broker"],
        port=mqtt_cfg["port"],
        auth=auth,
    )


@app.route("/api/command/water", methods=["POST"])
def api_command_water():
    """Send a manual watering command to an ESP device via MQTT.

    The target source can be passed in the JSON body as ``{"source": "esp32_01"}``.
    If omitted, defaults to ``esp8266`` for backward compatibility.
    """
    mqtt_cfg = _config.get("mqtt")
    if not mqtt_cfg:
        return jsonify({"error": "MQTT not configured"}), 503

    body = request.get_json(silent=True) or {}
    source = str(body.get("source") or "esp8266").strip().lower()
    if not source:
        source = "esp8266"

    topic_key = f"cmd_{source}"
    topics = mqtt_cfg.get("topics", {})
    topic = topics.get(topic_key)
    if not topic and source == "esp8266":
        topic = topics.get("cmd_esp8266", "commands/esp8266")
    if not topic:
        return jsonify({"error": f"Topic de comando no configurado para {source}"}), 400

    try:
        _mqtt_publish_command(mqtt_cfg, topic, {"action": "water"})
        return jsonify({"ok": True, "topic": topic, "source": source})
    except Exception as exc:
        # Log the full error server-side but do not expose internal details
        # (stack traces, hostnames, …) to the API caller to avoid information
        # leakage (CWE-209 / CodeQL py/stack-trace-exposure).
        logger.error("Failed to publish watering command: %s", exc)
        return jsonify({"error": "No se pudo enviar el comando al broker MQTT"}), 502


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def load_config(path: str = "config.json") -> dict:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="EM_server web dashboard")
    parser.add_argument(
        "--config",
        default="config.json",
        help="Path to the JSON configuration file (default: config.json)",
    )
    args = parser.parse_args()

    _config = load_config(args.config)
    _db_path = _config["database"]["path"]
    init_db(_db_path)

    web_cfg = _config["web"]
    app.secret_key = web_cfg["secret_key"]
    app.run(
        host=web_cfg["host"],
        port=web_cfg["port"],
        debug=web_cfg["debug"],
    )
