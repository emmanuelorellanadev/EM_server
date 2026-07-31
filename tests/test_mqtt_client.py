"""
tests/test_mqtt_client.py – Unit tests for field normalization in mqtt_client.py
"""
import pytest
import database
from mqtt_client import _on_message


class _FakeMsg:
    """Minimal stand-in for a paho MQTT message."""
    def __init__(self, topic: str, payload: bytes):
        self.topic = topic
        self.payload = payload


def _make_userdata(db_path: str, field_mappings: dict) -> dict:
    return {
        "db_path": db_path,
        "config": {"field_mappings": field_mappings},
    }


@pytest.fixture()
def db_path(tmp_path):
    path = str(tmp_path / "test.db")
    database.init_db(path)
    return path


def test_field_mapping_renames_soil_vwc_to_soil_humidity(db_path):
    """ESP8266 'soil_vwc' should be stored as 'soil_humidity' after mapping."""
    import json
    payload = json.dumps({
        "raw": 512, "soil_vwc": 42.3, "watering": False, "cooldown": False
    }).encode()
    userdata = _make_userdata(
        db_path,
        {"esp8266": {"soil_vwc": "soil_humidity", "raw": "soil_raw"}}
    )
    _on_message(None, userdata, _FakeMsg("sensors/esp8266", payload))

    rows = database.get_readings_history(db_path, source="esp8266")
    fields = {r["field"] for r in rows}
    assert "soil_humidity" in fields, "'soil_vwc' should be renamed to 'soil_humidity'"
    assert "soil_raw" in fields,      "'raw' should be renamed to 'soil_raw'"
    assert "soil_vwc" not in fields
    assert "raw" not in fields


def test_field_mapping_applies_to_esp32_source(db_path):
    """ESP32 source uses its own mapping and is stored under esp32_01."""
    import json
    payload = json.dumps({"soil_vwc": 55.7, "watering": True}).encode()
    userdata = _make_userdata(db_path, {"esp32_01": {"soil_vwc": "soil_humidity"}})

    _on_message(None, userdata, _FakeMsg("sensors/esp32_01", payload))

    rows = database.get_readings_history(db_path, source="esp32_01")
    fields = {r["field"] for r in rows}
    assert "soil_humidity" in fields
    assert "soil_vwc" not in fields


def test_no_mapping_stores_original_field_name(db_path):
    """With no field_mappings configured, field names pass through unchanged."""
    import json
    payload = json.dumps({"temperature": 24.5}).encode()
    userdata = _make_userdata(db_path, {})
    _on_message(None, userdata, _FakeMsg("sensors/raspberrypi", payload))

    rows = database.get_readings_history(db_path, source="raspberrypi")
    assert rows[0]["field"] == "temperature"


def test_invalid_json_payload_is_ignored(db_path):
    userdata = _make_userdata(db_path, {})
    _on_message(None, userdata, _FakeMsg("sensors/esp8266", b"not-json"))
    assert database.get_readings_history(db_path) == []


def test_device_status_online_is_stored_as_boolean(db_path):
    userdata = _make_userdata(db_path, {})
    _on_message(None, userdata, _FakeMsg("devices/esp8266/status", b"online"))
    rows = database.get_readings_history(db_path, source="esp8266")
    assert len(rows) == 1
    assert rows[0]["field"] == "online"
    assert rows[0]["value"] == 1.0


def test_device_status_offline_is_stored_as_boolean(db_path):
    userdata = _make_userdata(db_path, {})
    _on_message(None, userdata, _FakeMsg("devices/esp8266/status", b"offline"))
    rows = database.get_readings_history(db_path, source="esp8266")
    assert len(rows) == 1
    assert rows[0]["field"] == "online"
    assert rows[0]["value"] == 0.0
