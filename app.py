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
from flask import Flask, jsonify, render_template, request

import paho.mqtt.publish as mqtt_publish

from database import (
    get_latest_readings,
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
    "light":                "Iluminación",
    "pressure":             "Presión Atmosférica",
    "watering":             "Riego Activo",
    "on_threshold_percent": "Umbral de Activación (%)",
    "relay_on_time_s":      "Duración de Riego (s)",
}

# Fields that represent boolean on/off state (stored as 1.0 / 0.0)
BOOLEAN_FIELDS = {"watering"}


@app.template_global()
def field_icon(field: str) -> str:
    return _FIELD_ICONS.get(field, "📊")


@app.template_global()
def field_label(field: str) -> str:
    return _FIELD_LABELS.get(field, field.replace("_", " ").title())


@app.template_global()
def is_boolean_field(field: str) -> bool:
    return field in BOOLEAN_FIELDS

_config: dict = {}
_db_path: str = "em_server.db"


# ---------------------------------------------------------------------------
# HTML routes
# ---------------------------------------------------------------------------


@app.route("/")
def index():
    latest = get_latest_readings(_db_path)
    sources = get_sources(_db_path)
    return render_template("index.html", latest=latest, sources=sources)


@app.route("/history")
def history():
    source = request.args.get("source")
    field = request.args.get("field")
    limit = int(request.args.get("limit", 100))
    readings = get_readings_history(_db_path, source=source, field=field, limit=limit)
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
    hours_raw = request.args.get("hours")
    hours = int(hours_raw) if hours_raw else None
    return jsonify(
        get_readings_history(_db_path, source=source, field=field, limit=limit, hours=hours)
    )


@app.route("/api/sources")
def api_sources():
    return jsonify(get_sources(_db_path))


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
    """Send a manual watering command to the ESP8266 via MQTT.

    Publishes ``{"action": "water"}`` to the command topic configured in
    ``config.json → mqtt.topics.cmd_esp8266``.

    The ESP8266 subscribes to that topic (MQTT_TOPICO_CMD in config.h) and,
    upon receiving the command, activates the irrigation valve for
    DURACION_RIEGO_MS milliseconds, cancelling any active cooldown.

    Returns:
        200 {"ok": true, "topic": "commands/esp8266"}   — command sent
        503 {"error": "MQTT not configured"}             — no config loaded
        502 {"error": "<reason>"}                        — broker unreachable
    """
    mqtt_cfg = _config.get("mqtt")
    if not mqtt_cfg:
        return jsonify({"error": "MQTT not configured"}), 503

    topic = mqtt_cfg.get("topics", {}).get("cmd_esp8266", "commands/esp8266")
    try:
        _mqtt_publish_command(mqtt_cfg, topic, {"action": "water"})
        return jsonify({"ok": True, "topic": topic})
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
