"""Formatting helpers and field metadata for the dashboard UI.

Shared between the dashboard template globals and the API trend builder.
"""

from datetime import datetime
from typing import Optional
from zoneinfo import ZoneInfo

# Timezone used for display formatting (configurable at startup).
DISPLAY_TZ = ZoneInfo("America/Guatemala")

_FIELD_ICONS = {
    "ambient_temperature":  "🌡️",
    "ambient_humidity":     "💧",
    "soil_humidity":        "🌱",
    "online":               "📡",
    "light":                "☀️",
    "pressure":             "🌀",
    "watering":             "🚿",
    "on_threshold_soil_vwc": "🎯",
    "relay_on_time_s":       "⏱️",
}
_FIELD_LABELS = {
    "ambient_temperature":  "Temperatura Ambiental",
    "ambient_humidity":     "Humedad Ambiental",
    "soil_humidity":        "Humedad de Suelo",
    "online":               "Conectado MQTT",
    "light":                "Luz Ambiental",
    "pressure":             "Presión Atmosférica",
    "watering":             "Riego Activo",
    "on_threshold_soil_vwc": "Umbral de Activación (%)",
    "relay_on_time_s":      "Duración de Riego (s)",
}

# Fields stored as boolean state (1.0 / 0.0 in DB).
BOOLEAN_FIELDS = {"watering", "online"}
HIDDEN_FIELDS = {"last_watered_sec", "last_watering_at_epoch", "relay_on_time_s", "light_raw"}
NO_LAST_WATERING_TEXT = "sin registro"
NO_RELAY_DURATION_TEXT = "sin dato"
NO_THRESHOLD_TEXT = "sin dato"
LAST_WATERED_SEC_FIELD = "last_watered_sec"
LAST_WATERING_EPOCH_FIELD = "last_watering_at_epoch"
RELAY_ON_TIME_FIELD = "relay_on_time_s"
ON_THRESHOLD_FIELD = "on_threshold_soil_vwc"


def set_display_timezone(tz_name: str) -> None:
    """Update the timezone used for display formatting."""
    global DISPLAY_TZ
    DISPLAY_TZ = ZoneInfo(tz_name)


def field_icon(field: str) -> str:
    return _FIELD_ICONS.get(field, "📊")


def field_label(field: str) -> str:
    return _FIELD_LABELS.get(field, field.replace("_", " ").title())


def is_boolean_field(field: str) -> bool:
    return field in BOOLEAN_FIELDS


def should_render_field(field: str) -> bool:
    return field not in HIDDEN_FIELDS


def format_last_watering(epoch_value: float) -> str:
    """Build a user-friendly label for the latest watering timestamp."""
    try:
        epoch = float(epoch_value)
    except (TypeError, ValueError):
        return NO_LAST_WATERING_TEXT

    if epoch <= 0:
        return NO_LAST_WATERING_TEXT

    dt = datetime.fromtimestamp(epoch, DISPLAY_TZ)
    return dt.strftime('%d/%m/%Y %H:%M:%S')


def _recorded_at_to_epoch(recorded_at_value: str) -> Optional[float]:
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


def format_relay_duration(seconds_value: float) -> str:
    """Build a user-friendly label for relay-on duration in seconds."""
    try:
        seconds = float(seconds_value)
    except (TypeError, ValueError):
        return NO_RELAY_DURATION_TEXT

    if seconds < 0:
        return NO_RELAY_DURATION_TEXT

    if seconds.is_integer():
        return f"{int(seconds)} s"

    return f"{seconds:.1f} s"


def format_threshold(threshold_value: float) -> str:
    """Build a user-friendly label for watering threshold."""
    try:
        threshold = float(threshold_value)
    except (TypeError, ValueError):
        return NO_THRESHOLD_TEXT

    if threshold < 0:
        return NO_THRESHOLD_TEXT

    if threshold.is_integer():
        return f"{int(threshold)} %"

    return f"{threshold:.1f} %"
