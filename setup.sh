#!/usr/bin/env bash
# setup.sh – Installs and configures EM Server on Raspbian
# Usage: sudo bash setup.sh

set -euo pipefail

EM_DIR="/home/pi/EM_server"
VENV="$EM_DIR/venv"

echo "===================================================================="
echo " EM Server – Setup para Raspbian"
echo "===================================================================="

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

# ---- 4. Generate a random secret key in config.json --------------------
echo ""
echo "[4/6] Configurando config.json..."
SECRET=$(python3 -c "import secrets; print(secrets.token_hex(32))")
CONFIG="$EM_DIR/config.json"
if [ -f "$CONFIG" ]; then
    # Replace the placeholder secret key
    sed -i "s/change-this-secret-key-in-production/$SECRET/g" "$CONFIG"
    echo "      Secret key generado y guardado."
else
    echo "      ADVERTENCIA: $CONFIG no encontrado. Crea el archivo manualmente."
fi

# ---- 5. Install systemd services ----------------------------------------
echo ""
echo "[5/6] Instalando servicios systemd..."
cp "$EM_DIR/systemd/em-mqtt-client.service"    /etc/systemd/system/
cp "$EM_DIR/systemd/em-web-dashboard.service"  /etc/systemd/system/
cp "$EM_DIR/systemd/em-sensehat-client.service" /etc/systemd/system/

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
echo "   Dashboard web: http://$(hostname -I | awk '{print $1}'):5000"
echo ""
echo "   Servicios activos:"
echo "     systemctl status em-mqtt-client"
echo "     systemctl status em-sensehat-client"
echo "     systemctl status em-web-dashboard"
echo "===================================================================="
