"""
app.py – Flask web dashboard for EM_server.

Displays live and historical sensor readings received from the MQTT network.

Run:
    python app.py [--config config.json]
"""

import argparse
import json
import os
from flask import Flask, jsonify, render_template, request

from database import (
    get_latest_readings,
    get_readings_history,
    get_sources,
    init_db,
)

app = Flask(__name__)

# ---------------------------------------------------------------------------
# Template helpers
# ---------------------------------------------------------------------------

_FIELD_ICONS = {
    "temperature":   "🌡️",
    "humidity":      "💧",
    "soil_humidity": "🌱",
    "soil_raw":      "📟",
    "light":         "☀️",
    "pressure":      "🌀",
    "watering":      "🚿",
    "cooldown":      "⏳",
}
_FIELD_LABELS = {
    "temperature":   "Temperatura",
    "humidity":      "Humedad Ambiental",
    "soil_humidity": "Humedad de Suelo",
    "soil_raw":      "ADC Bruto",
    "light":         "Iluminación",
    "pressure":      "Presión Atmosférica",
    "watering":      "Riego Activo",
    "cooldown":      "En Cooldown",
}

# Fields that represent boolean on/off state (stored as 1.0 / 0.0)
BOOLEAN_FIELDS = {"watering", "cooldown"}


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
    return jsonify(get_readings_history(_db_path, source=source, field=field, limit=limit))


@app.route("/api/sources")
def api_sources():
    return jsonify(get_sources(_db_path))


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
