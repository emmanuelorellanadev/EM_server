"""Flask application factory for EM_server.

Builds the dashboard and API app from a configuration dict.

Run:
    python -m em_server [--config config.json]
"""

from flask import Flask

from em_server.routes.api import api_bp
from em_server.routes.dashboard import dashboard_bp
from em_server.utils.formatters import (
    field_icon,
    field_label,
    is_boolean_field,
    set_display_timezone,
    should_render_field,
)

DEFAULT_ESP_PANEL_SOURCES = ["esp8266", "esp32_01", "esp32_02"]


def create_app(config: dict) -> Flask:
    """Create and configure the Flask application.

    Args:
        config: Parsed configuration dict (see em_server.config.load_config).

    Returns:
        A configured Flask application ready to run or serve.
    """
    # Templates and static files live at the project root.
    app = Flask(__name__, template_folder="../templates", static_folder="../static")

    # Store app-specific settings.
    app.config["EM_CONFIG"] = config
    app.config["EM_DB_PATH"] = config["database"]["path"]
    app.config["EM_MQTT_CONFIG"] = config.get("mqtt")
    app.config["EM_DEFAULT_TREND_SOURCE"] = (
        config.get("web", {}).get("default_trend_source", "esp8266")
    )
    app.secret_key = config.get("web", {}).get("secret_key", "dev")

    web_cfg = config.get("web", {})
    set_display_timezone(web_cfg.get("display_timezone", "America/Guatemala"))
    esp_panel_sources = set(web_cfg.get("esp_panel_sources", DEFAULT_ESP_PANEL_SOURCES))

    # Register template globals.
    app.template_global()(field_icon)
    app.template_global()(field_label)
    app.template_global()(is_boolean_field)
    app.template_global()(should_render_field)

    @app.template_global()
    def is_esp_panel_source(source: str) -> bool:
        return source in esp_panel_sources

    # Register blueprints.
    app.register_blueprint(dashboard_bp)
    app.register_blueprint(api_bp)

    return app
