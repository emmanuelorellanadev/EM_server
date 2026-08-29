#!/usr/bin/env bash
# setup.sh – Installs and configures EM Server on Raspbian
# Usage: sudo bash deploy/setup.sh

set -euo pipefail

# Allow overrides via environment variables:
#   sudo SERVICE_USER=bitspi EM_DIR=/home/bitspi/Development/EM/EM_server bash deploy/setup.sh
DEFAULT_SERVICE_USER="${SUDO_USER:-${USER}}"
SERVICE_USER="${SERVICE_USER:-$DEFAULT_SERVICE_USER}"

# EM_DIR is the repository root; this script lives in EM_DIR/deploy/.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_EM_DIR="/home/$SERVICE_USER/Development/EM/EM_server"
LEGACY_EM_DIR="/home/$SERVICE_USER/EM_server"

if [ -z "${EM_DIR:-}" ]; then
    if [ -d "$DEFAULT_EM_DIR" ]; then
        EM_DIR="$DEFAULT_EM_DIR"
    elif [ -d "$LEGACY_EM_DIR" ]; then
        EM_DIR="$LEGACY_EM_DIR"
    else
        EM_DIR="$(dirname "$SCRIPT_DIR")"
    fi
fi

VENV="$EM_DIR/venv"

echo "===================================================================="
echo " EM Server – Setup para Raspbian"
echo "===================================================================="
echo "  SERVICE_USER: $SERVICE_USER"
echo "  EM_DIR:       $EM_DIR"

if [ ! -f "$EM_DIR/requirements.txt" ]; then
    echo "ERROR: No se encontro requirements.txt en: $EM_DIR"
    exit 1
fi

if [ ! -d "$EM_DIR/deploy/systemd" ]; then
    echo "ERROR: No se encontro carpeta deploy/systemd en: $EM_DIR"
    exit 1
fi

# ---- 1. System packages -------------------------------------------------
echo ""
echo "[1/6] Instalando paquetes del sistema..."
apt-get update -qq
apt-get install -y --no-install-recommends \
    mosquitto \
    mosquitto-clients \
    python3 \
    python3-pip \
    python3-venv \
    python3-sense-hat \
    sense-hat

# ---- 2. Enable and start Mosquitto MQTT broker --------------------------
echo ""
echo "[2/6] Habilitando y arrancando Mosquitto..."
systemctl enable mosquitto
systemctl start mosquitto

# ---- 3. Python virtual environment and dependencies --------------------
echo ""
echo "[3/6] Creando entorno virtual Python en $VENV..."
python3 -m venv --system-site-packages "$VENV"

echo "      Instalando dependencias Python..."
"$VENV/bin/pip" install --quiet --upgrade pip
"$VENV/bin/pip" install --quiet -r "$EM_DIR/requirements.txt"

# ---- 4. Generate config.json from template ---------------------------------
echo ""
echo "[4/6] Configurando config.json..."
SECRET=$(python3 -c "import secrets; print(secrets.token_hex(32))")
CONFIG="$EM_DIR/config.json"
CONFIG_EXAMPLE="$EM_DIR/config.example.json"

if [ ! -f "$CONFIG" ] && [ -f "$CONFIG_EXAMPLE" ]; then
    cp "$CONFIG_EXAMPLE" "$CONFIG"
    echo "      config.json creado desde config.example.json"
fi

if [ -f "$CONFIG" ]; then
    # Replace the placeholder secret key (supports both old and new placeholders)
    sed -i "s/change-this-secret-key-in-production/$SECRET/g" "$CONFIG"
    sed -i "s/CHANGE_THIS_IN_PRODUCTION/$SECRET/g" "$CONFIG"
    echo "      Secret key generado y guardado."
else
    echo "      ADVERTENCIA: $CONFIG no encontrado. Crea el archivo manualmente."
fi

# ---- 5. Install systemd services ----------------------------------------
echo ""
echo "[5/6] Instalando servicios systemd..."

install_service_from_template() {
    local template_path="$1"
    local target_path="/etc/systemd/system/$(basename "$template_path")"

    sed \
        -e "s|^User=.*$|User=$SERVICE_USER|" \
        -e "s|/home/pi/EM_server|$EM_DIR|g" \
        "$template_path" > "$target_path"
}

install_service_from_template "$EM_DIR/deploy/systemd/em-mqtt-client.service"
install_service_from_template "$EM_DIR/deploy/systemd/em-web-dashboard.service"
install_service_from_template "$EM_DIR/deploy/systemd/em-sensehat-client.service"

systemctl daemon-reload
systemctl enable em-mqtt-client em-web-dashboard em-sensehat-client

# ---- 6. Start services --------------------------------------------------
echo ""
echo "[6/6] Arrancando servicios..."
systemctl start em-mqtt-client
systemctl start em-sensehat-client
systemctl start em-web-dashboard

echo ""
echo "===================================================================="
echo " ✅  Instalación completa."
echo ""
echo "   Dashboard web: http://$(hostname -I | awk '{print $1}'):8080"
echo ""
echo "   Servicios activos:"
echo "     systemctl status em-mqtt-client"
echo "     systemctl status em-sensehat-client"
echo "     systemctl status em-web-dashboard"
echo "===================================================================="
