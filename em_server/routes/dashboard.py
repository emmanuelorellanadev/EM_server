"""Dashboard HTML routes for EM_server.

Renders the main dashboard and the history view, assembling the
template context from the latest SQLite readings.
"""

from typing import Optional

from flask import Blueprint, current_app, render_template, request

from em_server.models.database import (
    get_last_valid_last_watering_epoch,
    get_latest_readings,
    get_readings_history,
    get_sources,
)
from em_server.utils.formatters import (
    LAST_WATERED_SEC_FIELD,
    LAST_WATERING_EPOCH_FIELD,
    ON_THRESHOLD_FIELD,
    RELAY_ON_TIME_FIELD,
    _recorded_at_to_epoch,
    format_last_watering,
    format_relay_duration,
    format_threshold,
)

dashboard_bp = Blueprint("dashboard", __name__)


# ---------------------------------------------------------------------------
# Data assembly helpers
# ---------------------------------------------------------------------------


def build_latest_by_source(latest_readings: list[dict]) -> dict[str, dict[str, dict]]:
    """Index latest readings as ``source -> field -> row``.

    Avoids repeated scans from templates.
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
    db_path: str,
) -> dict[str, float]:
    """Resolve one epoch timestamp per source for "ultimo riego".

    Resolution order:
    1) latest ``last_watering_at_epoch``.
    2) derive from ``last_watered_sec`` + ``recorded_at``.
    3) DB fallback when ESP reports ``last_watered_sec = -1``.
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

        fallback_epoch = get_last_valid_last_watering_epoch(db_path, source)
        if fallback_epoch is not None and fallback_epoch > 0:
            by_source[source] = fallback_epoch

    return by_source


def build_last_watering_by_source(
    latest_by_source: dict[str, dict[str, dict]],
    db_path: str,
) -> tuple[dict[str, str], dict[str, float]]:
    """Build both label and raw epoch maps for card rendering."""
    epoch_by_source = build_last_watering_epoch_by_source(latest_by_source, db_path)
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


def _decorate_history_rows(readings: list[dict], db_path: str) -> list[dict]:
    """Add UI-friendly display fields for history rows.

    Keeps raw DB rows unchanged and adds display fields for templates.
    """
    # DB fallback is needed only for sources that report last_watered_sec.
    sources_needing_fallback = {
        row.get("source")
        for row in readings
        if row.get("field") == LAST_WATERED_SEC_FIELD
    }
    fallback_epoch_by_source: dict[str, float] = {}
    for source in sources_needing_fallback:
        if not source:
            continue
        epoch = get_last_valid_last_watering_epoch(db_path, source)
        if epoch is not None and epoch > 0:
            fallback_epoch_by_source[source] = epoch

    decorated: list[dict] = []
    for row in readings:
        # Copy to avoid mutating DB row data in-place.
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
                    current["display_value"] = "sin registro"
                current["display_unit"] = ""
                current["display_is_numeric"] = False

        decorated.append(current)

    return decorated


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------


@dashboard_bp.route("/")
def index():
    db_path = current_app.config["EM_DB_PATH"]
    latest = get_latest_readings(db_path)
    sources = get_sources(db_path)
    latest_by_source = build_latest_by_source(latest)
    last_watering_by_source, last_watering_epoch_by_source = build_last_watering_by_source(
        latest_by_source, db_path
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


@dashboard_bp.route("/history")
def history():
    db_path = current_app.config["EM_DB_PATH"]
    source = request.args.get("source")
    field = request.args.get("field")
    limit = int(request.args.get("limit", 100))
    readings = get_readings_history(db_path, source=source, field=field, limit=limit)
    readings = _decorate_history_rows(readings, db_path)
    sources = get_sources(db_path)
    return render_template(
        "history.html",
        readings=readings,
        sources=sources,
        selected_source=source,
        selected_field=field,
    )
