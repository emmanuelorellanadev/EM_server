"""Sense HAT MQTT publisher for EM_server.

Publishes temperature, humidity, and pressure from Sense HAT.
Temperature uses guardrails to reduce startup/outlier readings.

Run:
    python -m em_server.services.sensehat_service [--config config.json]
"""

import argparse
import json
import math
import signal
import statistics
import time
from typing import Optional

import paho.mqtt.client as mqtt
from sense_hat import SenseHat

from em_server.config import load_config
from em_server.utils.log_config import setup_logging

logger = setup_logging("sensehat_service")

_running = True


def _on_connect(client, userdata, flags, rc):
    if rc == 0:
        logger.info("Connected to MQTT broker")
    else:
        logger.error("Failed to connect to broker, rc=%d", rc)


def _read_sense_hat(sense: SenseHat) -> dict:
    """Read Sense HAT values with default temperature guardrails."""
    return _read_sense_hat_with_guardrails(
        sense,
        cpu_temp_correction=5.0,
        temp_min_c=-10.0,
        temp_max_c=65.0,
    )


def _read_sense_hat_with_guardrails(
    sense: SenseHat,
    cpu_temp_correction: float,
    temp_min_c: float,
    temp_max_c: float,
    last_valid_temperature_c: Optional[float] = None,
) -> dict:
    """Read Sense HAT sensors and apply sanity checks to temperature.

    Combines temperature getters and applies range checks.
    """
    temp_candidates: list[float] = []
    for getter_name in (
        "get_temperature_from_humidity",
        "get_temperature_from_pressure",
        "get_temperature",
    ):
        getter = getattr(sense, getter_name, None)
        if getter is None:
            continue
        try:
            value = float(getter())
        except Exception as exc:
            logger.warning("Temperature read failed (%s): %s", getter_name, exc)
            continue
        if math.isfinite(value):
            temp_candidates.append(value)

    if not temp_candidates:
        raise RuntimeError("No valid temperature readings from Sense HAT")

    # Median reduces the effect of a single faulty reading.
    temp_raw = statistics.median(temp_candidates)
    humidity = sense.get_humidity()
    pressure = sense.get_pressure()

    temperature = round(temp_raw - cpu_temp_correction, 2)
    if temperature < temp_min_c or temperature > temp_max_c:
        logger.warning(
            "Discarding out-of-range temperature %.2fC (raw=%.2fC, allowed=%.1f..%.1f)",
            temperature,
            temp_raw,
            temp_min_c,
            temp_max_c,
        )
        if last_valid_temperature_c is not None:
            temperature = round(last_valid_temperature_c, 2)
            logger.info("Reusing last valid temperature %.2fC", temperature)
        else:
            temperature = None

    payload = {
        "humidity":    {"value": round(humidity, 2),  "unit": "%"},
        "pressure":    {"value": round(pressure, 2),  "unit": "hPa"},
    }
    if temperature is not None:
        payload["temperature"] = {"value": temperature, "unit": "°C"}
    return payload


def run(config_path: str = "config.json") -> None:
    global _running
    config = load_config(config_path)
    mqtt_cfg = config["mqtt"]
    sh_cfg = config["sense_hat"]
    topic = sh_cfg["topic"]
    interval = sh_cfg["publish_interval_seconds"]
    cpu_temp_correction = float(sh_cfg.get("cpu_temp_correction", 5.0))
    temp_min_c = float(sh_cfg.get("temperature_min_c", -10.0))
    temp_max_c = float(sh_cfg.get("temperature_max_c", 65.0))

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
    logger.info(
        "Temperature guardrails: correction=%.2fC, range=%.1f..%.1fC",
        cpu_temp_correction,
        temp_min_c,
        temp_max_c,
    )

    last_valid_temperature_c: Optional[float] = None

    while _running:
        try:
            payload = _read_sense_hat_with_guardrails(
                sense,
                cpu_temp_correction=cpu_temp_correction,
                temp_min_c=temp_min_c,
                temp_max_c=temp_max_c,
                last_valid_temperature_c=last_valid_temperature_c,
            )
            temperature_entry = payload.get("temperature")
            if isinstance(temperature_entry, dict):
                temp_value = temperature_entry.get("value")
                if isinstance(temp_value, (int, float)) and math.isfinite(float(temp_value)):
                    last_valid_temperature_c = float(temp_value)
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
