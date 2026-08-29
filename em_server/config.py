"""Centralized configuration loader for EM_server.

Reads config.json and provides a single access point for all
configuration values used across the application.

Environment variables override config.json values.  Mapping:

    EM_MQTT_BROKER   -> mqtt.broker
    EM_MQTT_PORT     -> mqtt.port
    EM_MQTT_USERNAME -> mqtt.username
    EM_MQTT_PASSWORD -> mqtt.password
    EM_SECRET_KEY    -> web.secret_key
    EM_WEB_HOST      -> web.host
    EM_WEB_PORT      -> web.port
    EM_WEB_DEBUG     -> web.debug
    EM_DB_PATH       -> database.path
    EM_API_KEY       -> web.api_key
"""

import json
import os
from typing import Optional

_ENV_OVERRIDES: dict[str, tuple[str, str, Optional[type]]] = {
    "EM_MQTT_BROKER":   ("mqtt",      "broker",    None),
    "EM_MQTT_PORT":     ("mqtt",      "port",      int),
    "EM_MQTT_USERNAME": ("mqtt",      "username",  None),
    "EM_MQTT_PASSWORD": ("mqtt",      "password",  None),
    "EM_SECRET_KEY":    ("web",       "secret_key", None),
    "EM_WEB_HOST":      ("web",       "host",      None),
    "EM_WEB_PORT":      ("web",       "port",      int),
    "EM_WEB_DEBUG":     ("web",       "debug",     bool),
    "EM_DB_PATH":       ("database",  "path",      None),
    "EM_API_KEY":       ("web",       "api_key",   None),
}


def _coerce(value: str, target_type: Optional[type]) -> object:
    """Coerce a string environment variable to the expected type."""
    if target_type is bool:
        return value.lower() in ("true", "1", "yes")
    if target_type is not None:
        return target_type(value)
    return value


def load_config(path: str = "config.json") -> dict:
    """Load and return the JSON configuration file.

    Values set via environment variables take precedence over the JSON file.

    Args:
        path: Path to the JSON configuration file.

    Returns:
        Parsed configuration dictionary.

    Raises:
        FileNotFoundError: If the configuration file does not exist.
        json.JSONDecodeError: If the file contains invalid JSON.
    """
    with open(path, "r", encoding="utf-8") as fh:
        config = json.load(fh)

    for env_key, (section, key, target_type) in _ENV_OVERRIDES.items():
        raw = os.environ.get(env_key)
        if raw is not None:
            config.setdefault(section, {})[key] = _coerce(raw, target_type)

    return config
