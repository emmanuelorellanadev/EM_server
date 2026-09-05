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
import ssl
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
    """Build and configure an MQTT client with optional TLS/mTLS.

    When mqtt.tls.enabled is true in config.json, the client loads:
      - ca_cert:    CA root certificate to validate the broker
      - client_cert: This client's certificate (for mTLS authentication)
      - client_key:  This client's private key (proof of identity)

    The broker uses the CN from client_cert as the MQTT username (mTLS)
    and applies ACL rules based on that identity.

    Args:
        config: Parsed config.json dictionary.
        db_path: Path to the SQLite database file.

    Returns:
        Configured paho.mqtt.Client instance (not yet connected).
    """
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

    # ── TLS/mTLS configuration ──────────────────────────────────────
    # If tls.enabled is true in config.json, we configure the client to:
    #   1. Validate the broker's certificate using the CA root (tls_set)
    #   2. Present our own certificate for mTLS (certfile + keyfile)
    #   3. Optionally skip hostname verification (insecure, not recommended)
    #
    # This is required when the broker has "require_certificate true"
    # in its configuration (port 8883 with mTLS).
    tls_cfg = cfg.get("tls", {})
    if tls_cfg.get("enabled"):
        ca = tls_cfg["ca_cert"]
        cert = tls_cfg.get("client_cert")
        key = tls_cfg.get("client_key")
        try:
            client.tls_set(ca_certs=ca, certfile=cert, keyfile=key)
        except (FileNotFoundError, PermissionError, ssl.SSLError) as exc:
            logger.error(
                "TLS setup failed — check cert file permissions and paths. "
                "ca=%s cert=%s key=%s error=%s",
                ca, cert, key, exc,
            )
            raise SystemExit(1) from exc
        # insecure=True skips hostname verification — ONLY for development.
        # In production, always use False (default) to prevent MITM attacks.
        client.tls_insecure_set(bool(tls_cfg.get("insecure", False)))
        logger.info("TLS/mTLS enabled: ca=%s cert=%s key=%s", ca, cert, key)

    try:
        client.connect(cfg["broker"], cfg["port"], keepalive=60)
    except OSError as exc:
        logger.error(
            "MQTT connect failed: broker=%s port=%d error=%s",
            cfg["broker"], cfg["port"], exc,
        )
        raise SystemExit(1) from exc
    return client


def run(config_path: str = "config.json") -> None:
    global _running
    config = load_config(config_path)
    db_path = config["database"]["path"]
    init_db(db_path)
    logger.info("Database initialised at %s", db_path)

    try:
        client = build_client(config, db_path)
    except SystemExit:
        raise
    except Exception as exc:
        logger.error("Failed to build MQTT client: %s", exc)
        raise SystemExit(1) from exc

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
