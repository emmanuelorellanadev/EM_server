"""Central logging configuration for EM_server.

Creates the ``logs/`` directory if missing and configures the root logger
with both a console handler and a rotating file handler (``logs/em_server.log``).

Usage:
    from em_server.utils.log_config import setup_logging
    logger = setup_logging("app")
"""

import logging
import os
from logging.handlers import RotatingFileHandler

LOG_DIR = "logs"
LOG_FILE = "em_server.log"
LOG_MAX_BYTES = 1_000_000
LOG_BACKUP_COUNT = 5


def setup_logging(logger_name: str, level: int = logging.INFO) -> logging.Logger:
    """Configure the root logger once and return a named logger.

    Args:
        logger_name: Name for the returned logger (e.g. "app", "mqtt_service").
        level:       Minimum level logged to both console and file.

    Returns:
        A ``logging.Logger`` instance bound to ``logger_name``.
    """
    os.makedirs(LOG_DIR, exist_ok=True)

    fmt = logging.Formatter("%(asctime)s [%(levelname)s] %(name)s: %(message)s")

    root = logging.getLogger()
    if not root.handlers:
        console = logging.StreamHandler()
        console.setFormatter(fmt)
        root.addHandler(console)

        file_handler = RotatingFileHandler(
            os.path.join(LOG_DIR, LOG_FILE),
            maxBytes=LOG_MAX_BYTES,
            backupCount=LOG_BACKUP_COUNT,
            encoding="utf-8",
        )
        file_handler.setFormatter(fmt)
        root.addHandler(file_handler)

    root.setLevel(level)
    return logging.getLogger(logger_name)
