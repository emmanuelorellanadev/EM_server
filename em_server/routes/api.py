"""JSON API routes for EM_server.

Exposes latest readings, history, sources, trend data, and the
manual watering command endpoint.
"""

import json
from datetime import datetime, timedelta
from typing import Optional

import paho.mqtt.publish as mqtt_publish
from flask import Blueprint, current_app, jsonify, request

from em_server.models.database import (
    get_field_history,
    get_latest_readings,
    get_readings_history,
    get_sources,
)
from em_server.utils.formatters import DISPLAY_TZ
from em_server.utils.log_config import setup_logging

api_bp = Blueprint("api", __name__)

logger = setup_logging("api")


# Keep a shared tuple for ESP32 ambient-capable nodes so adding a new ESP32
# only requires one explicit source entry below.
ESP32_AMBIENT_TREND_FIELDS = (
    "soil_humidity",
    "on_threshold_soil_vwc",
    "ambient_temperature",
    "ambient_humidity",
    "light",
)

SOURCE_TREND_FIELDS: dict[str, tuple[str, ...]] = {
    "esp8266": ("soil_humidity", "on_threshold_soil_vwc"),
    "esp32_01": ESP32_AMBIENT_TREND_FIELDS,
    "esp32_02": ESP32_AMBIENT_TREND_FIELDS,
    "raspberrypi": ("temperature", "humidity", "pressure"),
}

# Trend range options used by /api/trend.
# bucket_seconds applies downsampling on long ranges.
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


# ---------------------------------------------------------------------------
# Trend helpers
# ---------------------------------------------------------------------------


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

    Supported fields are selected by source via SOURCE_TREND_FIELDS.
    """
    cfg = TREND_RANGE_CONFIG[range_key]
    source = (source or "").strip().lower()
    db_path = current_app.config["EM_DB_PATH"]
    default_trend_source = current_app.config.get("EM_DEFAULT_TREND_SOURCE", "esp8266")
    default_trend_fields = SOURCE_TREND_FIELDS.get(default_trend_source, ("soil_humidity",))

    now = datetime.now(DISPLAY_TZ)
    since = now - cfg["delta"]
    trend_fields = SOURCE_TREND_FIELDS.get(source, default_trend_fields)

    datasets: dict[str, list[dict]] = {}
    for field in trend_fields:
        rows = get_field_history(
            db_path,
            source=source,
            field=field,
            since=since,
            limit=None,
        )
        datasets[field] = _bucket_trend_points(rows, cfg["bucket_seconds"])

    return {
        "source": source,
        "range": range_key,
        "range_label": cfg["label"],
        "fields": list(trend_fields),
        "datasets": datasets,
    }


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------


@api_bp.route("/api/latest")
def api_latest():
    return jsonify(get_latest_readings(current_app.config["EM_DB_PATH"]))


@api_bp.route("/api/history")
def api_history():
    db_path = current_app.config["EM_DB_PATH"]
    source = request.args.get("source")
    field = request.args.get("field")
    limit = int(request.args.get("limit", 100))
    return jsonify(get_readings_history(db_path, source=source, field=field, limit=limit))


@api_bp.route("/api/sources")
def api_sources():
    return jsonify(get_sources(current_app.config["EM_DB_PATH"]))


@api_bp.route("/api/trend")
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
          "fields": ["soil_humidity", "on_threshold_soil_vwc"],
          "datasets": {
            "soil_humidity": [{"x": "...", "y": 45.2}],
            "on_threshold_soil_vwc": [{"x": "...", "y": 25.0}]
          }
        }
    """
    default_trend_source = current_app.config.get("EM_DEFAULT_TREND_SOURCE", "esp8266")
    source = (request.args.get("source") or default_trend_source).strip()
    range_key = (request.args.get("range") or "1h").strip().lower()

    if range_key not in TREND_RANGE_CONFIG:
        return jsonify({
            "error": "Invalid range",
            "valid_ranges": list(TREND_RANGE_CONFIG.keys()),
        }), 400

    return jsonify(_build_trend_response(source=source, range_key=range_key))


# ---------------------------------------------------------------------------
# Command API routes
# ---------------------------------------------------------------------------


def _mqtt_publish_command(mqtt_cfg: dict, topic: str, payload: dict) -> None:
    """Publish a single MQTT command message and disconnect immediately.

    Uses paho.mqtt.publish.single() for one-shot command publishing.
    Supports TLS/mTLS when mqtt.tls.enabled is configured.

    Args:
        mqtt_cfg: The ``mqtt`` block from config.json.
        topic:    MQTT topic to publish to (e.g. "commands/esp32_01").
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

    # ── TLS/mTLS configuration for one-shot publish ─────────────────
    # paho.mqtt.publish.single() accepts a "tls" dict parameter.
    # When tls.enabled is true, we pass the certificate paths so the
    # command is sent over the encrypted channel with client authentication.
    tls = None
    if mqtt_cfg.get("tls", {}).get("enabled"):
        tls = {
            "ca_certs": mqtt_cfg["tls"]["ca_cert"],
            "certfile": mqtt_cfg["tls"].get("client_cert"),
            "keyfile": mqtt_cfg["tls"].get("client_key"),
            "insecure": bool(mqtt_cfg["tls"].get("insecure", False)),
        }

    mqtt_publish.single(
        topic,
        payload=json.dumps(payload),
        qos=1,
        hostname=mqtt_cfg["broker"],
        port=mqtt_cfg["port"],
        auth=auth,
        tls=tls,
    )


@api_bp.route("/api/command/water", methods=["POST"])
def api_command_water():
    """Send a manual watering command to an ESP device via MQTT.

    The target source can be passed in the JSON body as
    ``{"source": "esp32_01"}`` or ``{"source": "esp32_02"}``.
    If omitted, defaults to ``esp8266`` for backward compatibility.
    """
    mqtt_cfg = current_app.config.get("EM_MQTT_CONFIG")
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
        logger.info("Water command published to %s (source=%s)", topic, source)
        return jsonify({"ok": True, "topic": topic, "source": source})
    except Exception as exc:
        # Log internal error details server-side only.
        logger.error("Failed to publish watering command to %s: %s", topic, exc)
        return jsonify({"error": "No se pudo enviar el comando al broker MQTT"}), 502
