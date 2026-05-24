"""
tests/test_app.py – Integration tests for the Flask web dashboard (app.py)
"""
import json
import pytest

# Patch database path before importing app
import database
import app as flask_app


@pytest.fixture()
def client(tmp_path, monkeypatch):
    db = str(tmp_path / "test.db")
    database.init_db(db)

    monkeypatch.setattr(flask_app, "_db_path", db)
    flask_app.app.config["TESTING"] = True
    flask_app.app.secret_key = "test-secret"

    with flask_app.app.test_client() as c:
        yield c, db


# ---------------------------------------------------------------------------
# HTML routes
# ---------------------------------------------------------------------------

def test_index_empty(client):
    c, _ = client
    resp = c.get("/")
    assert resp.status_code == 200
    assert b"EM Server" in resp.data


def test_index_with_data(client):
    c, db = client
    database.insert_reading(db, "esp8266", "temperature", 25.0, "°C")
    resp = c.get("/")
    assert resp.status_code == 200
    assert b"esp8266" in resp.data.lower()


def test_history_empty(client):
    c, _ = client
    resp = c.get("/history")
    assert resp.status_code == 200
    assert b"Historial" in resp.data


def test_history_with_filter(client):
    c, db = client
    database.insert_reading(db, "esp8266", "temperature", 25.0)
    resp = c.get("/history?source=esp8266&field=temperature&limit=50")
    assert resp.status_code == 200


# ---------------------------------------------------------------------------
# JSON API routes
# ---------------------------------------------------------------------------

def test_api_latest_empty(client):
    c, _ = client
    resp = c.get("/api/latest")
    assert resp.status_code == 200
    assert resp.get_json() == []


def test_api_latest_returns_data(client):
    c, db = client
    database.insert_reading(db, "esp8266", "humidity", 55.0, "%")
    resp = c.get("/api/latest")
    data = resp.get_json()
    assert len(data) == 1
    assert data[0]["source"] == "esp8266"
    assert data[0]["field"] == "humidity"


def test_api_history_empty(client):
    c, _ = client
    resp = c.get("/api/history")
    assert resp.status_code == 200
    assert resp.get_json() == []


def test_api_history_with_filter(client):
    c, db = client
    database.insert_reading(db, "esp8266", "temperature", 22.0)
    database.insert_reading(db, "raspberrypi", "temperature", 24.0)
    resp = c.get("/api/history?source=esp8266")
    data = resp.get_json()
    assert all(r["source"] == "esp8266" for r in data)


def test_api_sources_empty(client):
    c, _ = client
    resp = c.get("/api/sources")
    assert resp.status_code == 200
    assert resp.get_json() == []


def test_api_sources_returns_sources(client):
    c, db = client
    database.insert_reading(db, "esp8266",     "temperature", 22.0)
    database.insert_reading(db, "raspberrypi", "humidity",    55.0)
    resp = c.get("/api/sources")
    data = resp.get_json()
    assert set(data) == {"esp8266", "raspberrypi"}


def test_api_trend_default_range(client):
    c, db = client
    database.insert_reading(db, "esp8266", "soil_humidity", 41.0, "%")
    database.insert_reading(db, "esp8266", "on_threshold_percent", 25.0, "%")

    resp = c.get("/api/trend")
    assert resp.status_code == 200
    data = resp.get_json()

    assert data["source"] == "esp8266"
    assert data["range"] == "1h"
    assert "soil_humidity" in data["datasets"]
    assert "on_threshold_percent" in data["datasets"]


def test_api_trend_invalid_range_returns_400(client):
    c, _ = client
    resp = c.get("/api/trend?range=invalido")
    assert resp.status_code == 400
    body = resp.get_json()
    assert body["error"] == "Invalid range"
    assert "1h" in body["valid_ranges"]


def test_api_trend_filters_by_source(client):
    c, db = client
    database.insert_reading(db, "esp8266", "soil_humidity", 40.0, "%")
    database.insert_reading(db, "esp8266", "on_threshold_percent", 25.0, "%")
    database.insert_reading(db, "raspberrypi", "soil_humidity", 99.0, "%")

    resp = c.get("/api/trend?source=esp8266&range=1h")
    assert resp.status_code == 200
    data = resp.get_json()

    values = [p["y"] for p in data["datasets"]["soil_humidity"]]
    assert 99.0 not in values


def test_api_trend_supports_1d_range(client):
    c, db = client
    database.insert_reading(db, "esp8266", "soil_humidity", 52.0, "%")
    database.insert_reading(db, "esp8266", "on_threshold_percent", 25.0, "%")

    resp = c.get("/api/trend?range=1d")
    assert resp.status_code == 200
    data = resp.get_json()
    assert data["range"] == "1d"
    assert data["range_label"] == "Ultimo dia"


# ---------------------------------------------------------------------------
# Boolean fields rendered as status badges
# ---------------------------------------------------------------------------

def test_index_shows_watering_status(client):
    c, db = client
    database.insert_reading(db, "esp8266", "watering", 1.0)
    resp = c.get("/")
    assert resp.status_code == 200
    assert b"status-on" in resp.data


def test_index_shows_online_status(client):
    c, db = client
    database.insert_reading(db, "esp8266", "online", 1.0)
    resp = c.get("/")
    assert resp.status_code == 200
    assert b"Conectado MQTT" in resp.data
    assert b"status-on" in resp.data


def test_api_latest_includes_boolean_fields(client):
    c, db = client
    database.insert_reading(db, "esp8266", "watering", 0.0)
    database.insert_reading(db, "esp8266", "online", 1.0)
    database.insert_reading(db, "esp8266", "cooldown", 1.0)
    data = c.get("/api/latest").get_json()
    fields = {r["field"] for r in data}
    assert "watering" in fields
    assert "online" in fields
    assert "cooldown" in fields
    online_row = next(r for r in data if r["field"] == "online")
    assert online_row["value"] in (0.0, 1.0)


# ---------------------------------------------------------------------------
# Remote watering command — POST /api/command/water
# ---------------------------------------------------------------------------

def test_api_command_water_no_config(client, monkeypatch):
    """Returns 503 when no MQTT config is loaded (empty _config)."""
    c, _ = client
    monkeypatch.setattr(flask_app, "_config", {})  # explicit empty config
    resp = c.post("/api/command/water")
    assert resp.status_code == 503
    assert resp.get_json()["error"] == "MQTT not configured"


def test_api_command_water_publishes(client, monkeypatch):
    """Returns 200 and calls the MQTT publish helper with the correct arguments."""
    c, _ = client
    monkeypatch.setattr(flask_app, "_config", {
        "mqtt": {
            "broker": "localhost",
            "port": 1883,
            "username": "",
            "password": "",
            "topics": {"cmd_esp8266": "commands/esp8266"},
        }
    })
    published = []
    monkeypatch.setattr(flask_app, "_mqtt_publish_command",
                        lambda cfg, topic, payload: published.append((topic, payload)))

    resp = c.post("/api/command/water")
    assert resp.status_code == 200
    data = resp.get_json()
    assert data["ok"] is True
    assert data["topic"] == "commands/esp8266"
    assert len(published) == 1
    assert published[0] == ("commands/esp8266", {"action": "water"})


def test_api_command_water_broker_error(client, monkeypatch):
    """Returns 502 with a generic error message when the MQTT broker is unreachable."""
    c, _ = client
    monkeypatch.setattr(flask_app, "_config", {
        "mqtt": {
            "broker": "unreachable-host",
            "port": 1883,
            "username": "",
            "password": "",
            "topics": {"cmd_esp8266": "commands/esp8266"},
        }
    })

    def _fail(*args, **kwargs):
        raise ConnectionRefusedError("broker offline")

    monkeypatch.setattr(flask_app, "_mqtt_publish_command", _fail)

    resp = c.post("/api/command/water")
    assert resp.status_code == 502
    # The response must NOT expose internal details (stack traces, hostnames, etc.)
    body = resp.get_json()
    assert "error" in body
    assert "broker offline" not in body["error"]  # internal details hidden
    assert "unreachable-host" not in body["error"]
