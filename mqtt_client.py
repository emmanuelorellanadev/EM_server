"""
mqtt_client.py – MQTT subscriber service for EM_server.

Subscribes to all sensor topics, parses incoming JSON payloads,
and persists the data using the database module.

Run as a standalone process:
    python mqtt_client.py [--config config.json]
"""

import argparse
import json
import logging
import os
import signal
import sys
import time

import paho.mqtt.client as mqtt

from database import init_db, insert_reading, insert_readings_from_payload

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger("mqtt_client")

_running = True


def _load_config(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


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

        # Derive a human-readable source name from the topic path.
        # e.g. "sensors/esp8266" -> "esp8266"
        #      "sensors/raspberrypi" -> "raspberrypi"
        parts = topic.split("/")
        source = parts[-1] if len(parts) >= 2 else topic

    # Apply per-source field name mappings defined in config.json.
    # Example mapping: esp8266 "percent" -> "soil_humidity"
    mappings = userdata["config"].get("field_mappings", {}).get(source, {})
    if mappings:
        payload = {mappings.get(k, k): v for k, v in payload.items()}

    logger.debug("Message from %s: %s", source, payload)
    last_watered_sec = payload.get("last_watered_sec")
    if isinstance(last_watered_sec, (int, float)):
        if last_watered_sec >= 0:
            now_ts = time.time()
            last_watering_at_epoch = now_ts - float(last_watered_sec)
        else:
            # Sentinel used by ESP8266 when no irrigation has happened yet.
            last_watering_at_epoch = -1.0
        insert_reading(
            db_path,
            source,
            "last_watering_at_epoch",
            last_watering_at_epoch,
            "unix_s",
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
    config = _load_config(config_path)
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
