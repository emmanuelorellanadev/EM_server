"""Centralized configuration loader for EM_server.

Reads config.json and provides a single access point for all
configuration values used across the application.
"""

import json


def load_config(path: str = "config.json") -> dict:
    """Load and return the JSON configuration file.

    Args:
        path: Path to the JSON configuration file.

    Returns:
        Parsed configuration dictionary.

    Raises:
        FileNotFoundError: If the configuration file does not exist.
        json.JSONDecodeError: If the file contains invalid JSON.
    """
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)
