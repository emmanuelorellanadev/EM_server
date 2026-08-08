"""Entry point for ``python -m em_server``."""

import argparse

from em_server.config import load_config
from em_server.app import create_app
from em_server.models.database import init_db
from em_server.utils.log_config import setup_logging

logger = setup_logging("em_server")


def main() -> None:
    parser = argparse.ArgumentParser(description="EM_server web dashboard")
    parser.add_argument(
        "--config",
        default="config.json",
        help="Path to the JSON configuration file (default: config.json)",
    )
    args = parser.parse_args()

    config = load_config(args.config)
    db_path = config["database"]["path"]
    init_db(db_path)
    logger.info("Database initialised at %s", db_path)

    app = create_app(config)
    web_cfg = config["web"]
    logger.info(
        "Starting EM Server web dashboard on %s:%s",
        web_cfg["host"],
        web_cfg["port"],
    )
    app.run(
        host=web_cfg["host"],
        port=web_cfg["port"],
        debug=web_cfg["debug"],
    )


if __name__ == "__main__":
    main()
