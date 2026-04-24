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


# ---------------------------------------------------------------------------
# Boolean fields rendered as status badges
# ---------------------------------------------------------------------------

def test_index_shows_watering_status(client):
    c, db = client
    database.insert_reading(db, "esp8266", "watering", 1.0)
    resp = c.get("/")
    assert resp.status_code == 200
    assert b"status-on" in resp.data


def test_api_latest_includes_boolean_fields(client):
    c, db = client
    database.insert_reading(db, "esp8266", "watering", 0.0)
    database.insert_reading(db, "esp8266", "cooldown", 1.0)
    data = c.get("/api/latest").get_json()
    fields = {r["field"] for r in data}
    assert "watering" in fields
    assert "cooldown" in fields
