"""MQTT subscriber service for EM_server.

Flow:
1) Subscribe to sensor and status topics.
2) Decode payloads and normalize source name.
3) Apply optional field mappings from config.
4) Persist readings in SQLite.

Run:
    python -m em_server.services.mqtt_service [--config config.json]
"""

import argparse
import json
import signal
import time

import paho.mqtt.client as mqtt

from em_server.config import load_config
from em_server.models.database import init_db, insert_reading, insert_readings_from_payload
from em_server.utils.log_config import setup_logging

logger = setup_logging("mqtt_service")

_running = True


def _on_connect(client, userdata, flags, rc):
    if rc == 0:
        logger.info("Connected to MQTT broker")
        topics_cfg = userdata["config"]["mqtt"]["topics"]
        sensor_topic = topics_cfg["all"]
        status_topic = topics_cfg.get("device_status", "devices/+/status")
        client.subscribe(sensor_topic)
        client.subscribe(status_topic)
        logger.info("Subscribed to topic: %s", sensor_topic)
        logger.info("Subscribed to topic: %s", status_topic)
    else:
        logger.error("Failed to connect, return code %d", rc)


def _is_device_status_topic(topic: str) -> bool:
    parts = topic.split("/")
    return len(parts) == 3 and parts[0] == "devices" and parts[2] == "status"


def _on_message(client, userdata, msg):
    db_path = userdata["db_path"]
    topic = msg.topic
    if _is_device_status_topic(topic):
        try:
            status = msg.payload.decode("utf-8").strip().lower()
        except UnicodeDecodeError as exc:
            logger.warning("Could not decode status payload on %s: %s", topic, exc)
            return

        if status not in {"online", "offline"}:
            logger.warning("Ignoring unsupported status payload on %s: %r", topic, status)
            return

        source = topic.split("/")[1]
        payload = {"online": status == "online"}
    else:
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            logger.warning("Could not decode payload on %s: %s", topic, exc)
            return

        # Source is the last path segment in sensor topics.
        parts = topic.split("/")
        source = parts[-1] if len(parts) >= 2 else topic

    # Normalize source for deterministic matching across config/UI/API.
    source = str(source).strip().lower()
    if not source:
        logger.warning("Ignoring payload with empty source. topic=%s", topic)
        return

    # Apply optional field name mapping per source.
    mappings = userdata["config"].get("field_mappings", {}).get(source, {})
    if mappings:
        payload = {mappings.get(k, k): v for k, v in payload.items()}

    logger.info("Message from %s on %s: %s", source, topic, payload)
    if source.startswith("esp") and "last_watered_sec" not in payload:
        logger.warning(
            "Payload %s sin 'last_watered_sec'. keys=%s payload=%s",
            source,
            sorted(payload.keys()),
            payload,
        )

    raw_last_watered_sec = payload.get("last_watered_sec")
    last_watered_sec = None
    if isinstance(raw_last_watered_sec, (int, float)):
        last_watered_sec = float(raw_last_watered_sec)
    elif isinstance(raw_last_watered_sec, str):
        try:
            last_watered_sec = float(raw_last_watered_sec.strip())
        except ValueError:
            logger.warning(
                "Invalid 'last_watered_sec' from %s: %r",
                source,
                raw_last_watered_sec,
            )

    if last_watered_sec is not None:
        if last_watered_sec >= 0:
            now_ts = time.time()
            last_watering_at_epoch = now_ts - last_watered_sec
            insert_reading(
                db_path,
                source,
                "last_watering_at_epoch",
                last_watering_at_epoch,
                "unix_s",
            )
        else:
            # Keep previous last_watering_at_epoch on sentinel -1.
            logger.info(
                "Ignoring sentinel last_watered_sec=-1 from %s; "
                "preserving previous last_watering_at_epoch",
                source,
            )
    insert_readings_from_payload(db_path, source, payload)
    logger.info("Stored reading from source '%s'", source)


def _on_disconnect(client, userdata, rc):
    if rc != 0:
        logger.warning("Unexpected disconnection (rc=%d); will retry…", rc)


def build_client(config: dict, db_path: str) -> mqtt.Client:
    cfg = config["mqtt"]
    client = mqtt.Client(
        client_id=cfg["client_id_subscriber"],
        userdata={"config": config, "db_path": db_path},
    )
    client.on_connect = _on_connect
    client.on_message = _on_message
    client.on_disconnect = _on_disconnect

    if cfg.get("username"):
        client.username_pw_set(cfg["username"], cfg.get("password", ""))

    client.connect(cfg["broker"], cfg["port"], keepalive=60)
    return client


def run(config_path: str = "config.json") -> None:
    global _running
    config = load_config(config_path)
    db_path = config["database"]["path"]
    init_db(db_path)
    logger.info("Database initialised at %s", db_path)

    client = build_client(config, db_path)

    def _handle_signal(signum, frame):
        global _running
        logger.info("Received signal %d, shutting down…", signum)
        _running = False

    signal.signal(signal.SIGINT, _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)

    client.loop_start()
    logger.info("MQTT subscriber running. Press Ctrl+C to stop.")
    while _running:
        time.sleep(1)

    client.loop_stop()
    client.disconnect()
    logger.info("MQTT subscriber stopped.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="EM_server MQTT subscriber")
    parser.add_argument(
        "--config",
        default="config.json",
        help="Path to the JSON configuration file (default: config.json)",
    )
    args = parser.parse_args()
    run(args.config)
