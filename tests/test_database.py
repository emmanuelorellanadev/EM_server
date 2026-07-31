"""
tests/test_database.py – Unit tests for database.py
"""
import os
import pytest

from database import (
    init_db,
    insert_reading,
    insert_readings_from_payload,
    get_latest_readings,
    get_readings_history,
    get_sources,
)


@pytest.fixture()
def db_path(tmp_path):
    path = str(tmp_path / "test.db")
    init_db(path)
    return path


# ---------------------------------------------------------------------------
# init_db
# ---------------------------------------------------------------------------

def test_init_db_creates_file(tmp_path):
    path = str(tmp_path / "new.db")
    init_db(path)
    assert os.path.exists(path)


def test_init_db_idempotent(db_path):
    """Calling init_db twice should not raise."""
    init_db(db_path)


# ---------------------------------------------------------------------------
# insert_reading
# ---------------------------------------------------------------------------

def test_insert_reading_stores_row(db_path):
    insert_reading(db_path, "esp8266", "temperature", 25.3, "°C")
    rows = get_readings_history(db_path)
    assert len(rows) == 1
    r = rows[0]
    assert r["source"] == "esp8266"
    assert r["field"] == "temperature"
    assert abs(r["value"] - 25.3) < 0.001
    assert r["unit"] == "°C"


def test_insert_reading_default_unit(db_path):
    insert_reading(db_path, "raspberrypi", "humidity", 55.0)
    rows = get_readings_history(db_path)
    assert rows[0]["unit"] == ""


# ---------------------------------------------------------------------------
# insert_readings_from_payload
# ---------------------------------------------------------------------------

def test_payload_with_dict_values(db_path):
    payload = {
        "temperature":   {"value": 23.5, "unit": "°C"},
        "humidity":      {"value": 60.1, "unit": "%"},
        "soil_humidity": {"value": 42.0, "unit": "%"},
        "light":         {"value": 320,  "unit": "lux"},
        "pressure":      {"value": 1013, "unit": "hPa"},
    }
    insert_readings_from_payload(db_path, "esp8266", payload)
    rows = get_readings_history(db_path, source="esp8266")
    assert len(rows) == 5
    fields = {r["field"] for r in rows}
    assert fields == {"temperature", "humidity", "soil_humidity", "light", "pressure"}


def test_payload_with_scalar_values(db_path):
    payload = {"temperature": 22.1, "humidity": 55.5}
    insert_readings_from_payload(db_path, "esp8266", payload)
    rows = get_readings_history(db_path)
    assert len(rows) == 2
    for r in rows:
        assert r["unit"] == ""


def test_payload_skips_non_numeric(db_path):
    payload = {"temperature": 22.1, "label": "outdoor"}
    insert_readings_from_payload(db_path, "esp8266", payload)
    rows = get_readings_history(db_path)
    fields = {r["field"] for r in rows}
    assert "label" not in fields


def test_payload_boolean_stored_as_numeric(db_path):
    """Boolean values (from ESP8266 watering/cooldown) should be stored as 0.0/1.0."""
    payload = {"watering": True, "cooldown": False}
    insert_readings_from_payload(db_path, "esp8266", payload)
    rows = get_readings_history(db_path)
    by_field = {r["field"]: r["value"] for r in rows}
    assert by_field["watering"] == 1.0
    assert by_field["cooldown"] == 0.0


def test_payload_esp8266_full(db_path):
    """Full ESP8266 payload matches expected fields."""
    payload = {
        "raw":      512,
        "soil_vwc": 42.3,
        "state":    "MOIST",  # string – should be skipped
        "watering": False,
        "cooldown": False,
    }
    insert_readings_from_payload(db_path, "esp8266", payload)
    rows = get_readings_history(db_path, source="esp8266")
    fields = {r["field"] for r in rows}
    # state is a string → skipped; raw, soil_vwc, watering, cooldown stored
    assert "state" not in fields
    assert {"raw", "soil_vwc", "watering", "cooldown"} == fields


# ---------------------------------------------------------------------------
# get_latest_readings
# ---------------------------------------------------------------------------

def test_latest_returns_most_recent(db_path):
    insert_reading(db_path, "esp8266", "temperature", 20.0, "°C")
    insert_reading(db_path, "esp8266", "temperature", 25.0, "°C")
    latest = get_latest_readings(db_path)
    # Only one entry per (source, field)
    assert len(latest) == 1
    assert abs(latest[0]["value"] - 25.0) < 0.001


def test_latest_multiple_sources(db_path):
    insert_reading(db_path, "esp8266",     "temperature", 22.0, "°C")
    insert_reading(db_path, "raspberrypi", "temperature", 24.0, "°C")
    latest = get_latest_readings(db_path)
    assert len(latest) == 2
    sources = {r["source"] for r in latest}
    assert sources == {"esp8266", "raspberrypi"}


# ---------------------------------------------------------------------------
# get_readings_history
# ---------------------------------------------------------------------------

def test_history_filter_by_source(db_path):
    insert_reading(db_path, "esp8266",     "temperature", 22.0)
    insert_reading(db_path, "raspberrypi", "temperature", 24.0)
    rows = get_readings_history(db_path, source="esp8266")
    assert all(r["source"] == "esp8266" for r in rows)


def test_history_filter_by_field(db_path):
    insert_reading(db_path, "esp8266", "temperature", 22.0)
    insert_reading(db_path, "esp8266", "humidity",    60.0)
    rows = get_readings_history(db_path, field="temperature")
    assert all(r["field"] == "temperature" for r in rows)


def test_history_limit(db_path):
    for i in range(20):
        insert_reading(db_path, "esp8266", "temperature", float(i))
    rows = get_readings_history(db_path, limit=5)
    assert len(rows) == 5


# ---------------------------------------------------------------------------
# get_sources
# ---------------------------------------------------------------------------

def test_get_sources_empty(db_path):
    assert get_sources(db_path) == []


def test_get_sources_multiple(db_path):
    insert_reading(db_path, "esp8266",     "temperature", 22.0)
    insert_reading(db_path, "raspberrypi", "humidity",    55.0)
    sources = get_sources(db_path)
    assert set(sources) == {"esp8266", "raspberrypi"}
