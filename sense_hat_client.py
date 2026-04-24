"""
sense_hat_client.py – Publishes Raspberry Pi Sense HAT readings via MQTT.

Run as a standalone process on the Raspberry Pi:
    python sense_hat_client.py [--config config.json]

The Sense HAT provides: temperature, humidity, and pressure.
Readings are published as a JSON payload to the configured MQTT topic every
N seconds (configurable via config.json → sense_hat.publish_interval_seconds).
"""

import argparse
import json
import logging
import signal
import sys
import time

import paho.mqtt.client as mqtt
from sense_hat import SenseHat

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger("sense_hat_client")

_running = True


def _load_config(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def _on_connect(client, userdata, flags, rc):
    if rc == 0:
        logger.info("Connected to MQTT broker")
    else:
        logger.error("Failed to connect to broker, rc=%d", rc)


def _read_sense_hat(sense: SenseHat) -> dict:
    """Read all available Sense HAT sensors and return a payload dict."""
    # The Sense HAT temperature sensor is affected by CPU heat.
    # A common correction subtracts ~5 °C, but the exact offset depends on
    # the case/ventilation.  Adjust CPU_TEMP_CORRECTION for your setup.
    CPU_TEMP_CORRECTION = 5.0

    temp_raw = sense.get_temperature()
    humidity = sense.get_humidity()
    pressure = sense.get_pressure()

    temperature = round(temp_raw - CPU_TEMP_CORRECTION, 2)

    return {
        "temperature": {"value": temperature, "unit": "°C"},
        "humidity":    {"value": round(humidity, 2),  "unit": "%"},
        "pressure":    {"value": round(pressure, 2),  "unit": "hPa"},
    }


def run(config_path: str = "config.json") -> None:
    global _running
    config = _load_config(config_path)
    mqtt_cfg = config["mqtt"]
    sh_cfg = config["sense_hat"]
    topic = sh_cfg["topic"]
    interval = sh_cfg["publish_interval_seconds"]

    client = mqtt.Client(client_id=mqtt_cfg["client_id_sensehat"])
    client.on_connect = _on_connect
    if mqtt_cfg.get("username"):
        client.username_pw_set(mqtt_cfg["username"], mqtt_cfg.get("password", ""))

    client.connect(mqtt_cfg["broker"], mqtt_cfg["port"], keepalive=60)
    client.loop_start()

    sense = SenseHat()
    sense.clear()

    def _handle_signal(signum, frame):
        global _running
        logger.info("Received signal %d, shutting down…", signum)
        _running = False

    signal.signal(signal.SIGINT, _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)

    logger.info(
        "Sense HAT publisher running – publishing to '%s' every %ds.",
        topic,
        interval,
    )

    while _running:
        try:
            payload = _read_sense_hat(sense)
            client.publish(topic, json.dumps(payload), qos=1)
            logger.info("Published: %s", payload)
        except Exception as exc:
            logger.error("Error reading Sense HAT: %s", exc)
        time.sleep(interval)

    client.loop_stop()
    client.disconnect()
    logger.info("Sense HAT publisher stopped.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="EM_server Sense HAT MQTT publisher"
    )
    parser.add_argument(
        "--config",
        default="config.json",
        help="Path to the JSON configuration file (default: config.json)",
    )
    args = parser.parse_args()
    run(args.config)
