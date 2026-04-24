# EM Server – Monitoreo Meteorológico IoT

Sistema de monitoreo que **recibe, almacena y visualiza** datos de sensores meteorológicos transmitidos por MQTT desde un **ESP8266** y una **Raspberry Pi Sense HAT**.

---

## Arquitectura

```
┌──────────────┐          ┌──────────────────┐          ┌────────────────────┐
│   ESP8266    │─ MQTT ──►│  Mosquitto Broker │◄─ MQTT ─│ Sense HAT Publisher│
│ (soil, temp, │          │  (localhost:1883) │          │  (sense_hat_client)│
│  humidity,   │          └────────┬─────────┘          └────────────────────┘
│  light)      │                   │
└──────────────┘                   │ subscribe (sensors/#)
                                   ▼
                        ┌──────────────────────┐
                        │  mqtt_client.py       │
                        │  (MQTT subscriber)    │
                        └──────────┬───────────┘
                                   │ INSERT
                                   ▼
                        ┌──────────────────────┐
                        │  SQLite Database      │
                        │  (em_server.db)       │
                        └──────────┬───────────┘
                                   │ SELECT
                                   ▼
                        ┌──────────────────────┐
                        │  Flask Web Dashboard  │
                        │  (app.py :5000)       │
                        └──────────────────────┘
```

---

## Estructura del proyecto

```
EM_server/
├── config.json              # Configuración central (MQTT, DB, Web)
├── requirements.txt         # Dependencias Python
├── setup.sh                 # Script de instalación automática (Raspbian)
│
├── database.py              # Capa de acceso a SQLite
├── mqtt_client.py           # Servicio suscriptor MQTT → DB
├── sense_hat_client.py      # Publicador de datos del Sense HAT → MQTT
├── app.py                   # Dashboard web Flask
│
├── templates/
│   ├── base.html            # Plantilla base
│   ├── index.html           # Dashboard con lecturas actuales y gráficas
│   └── history.html         # Tabla de historial con filtros
│
├── static/
│   ├── css/style.css        # Estilos del dashboard
│   └── js/dashboard.js      # Gráficas (Chart.js) y auto-refresh
│
├── systemd/
│   ├── em-mqtt-client.service       # Servicio MQTT subscriber
│   ├── em-sensehat-client.service   # Servicio Sense HAT publisher
│   └── em-web-dashboard.service     # Servicio Flask
│
└── tests/
    ├── test_database.py     # Tests unitarios de database.py
    └── test_app.py          # Tests de integración de app.py
```

---

## Instalación rápida en Raspbian

```bash
# 1. Clona el repositorio
git clone https://github.com/emmanuelorellanadev/EM_server.git /home/pi/EM_server
cd /home/pi/EM_server

# 2. (Opcional) Ajusta config.json con la dirección del broker y credenciales

# 3. Ejecuta el script de instalación como root
sudo bash setup.sh
```

El script instala Mosquitto, crea el entorno virtual Python, configura la clave
secreta Flask y habilita los tres servicios systemd.

---

## Instalación manual

### 1. Dependencias del sistema

```bash
sudo apt-get update
sudo apt-get install -y mosquitto mosquitto-clients python3 python3-venv python3-sense-hat
sudo systemctl enable --now mosquitto
```

### 2. Entorno Python

```bash
python3 -m venv --system-site-packages venv
source venv/bin/activate
pip install -r requirements.txt
```

### 3. Iniciar los servicios

```bash
# Terminal 1 – Suscriptor MQTT (almacena en SQLite)
python mqtt_client.py --config config.json

# Terminal 2 – Publicador Sense HAT
python sense_hat_client.py --config config.json

# Terminal 3 – Dashboard web
python app.py --config config.json
```

Abre `http://<IP_de_la_Raspberry>:5000` en el navegador.

---

## Configuración (config.json)

| Clave | Descripción | Default |
|---|---|---|
| `mqtt.broker` | Dirección del broker MQTT | `localhost` |
| `mqtt.port` | Puerto del broker | `1883` |
| `mqtt.username` / `password` | Credenciales (vacío = sin auth) | `""` |
| `database.path` | Ruta del archivo SQLite | `em_server.db` |
| `web.port` | Puerto del dashboard Flask | `5000` |
| `sense_hat.publish_interval_seconds` | Intervalo de publicación del Sense HAT | `30` |

---

## Formato del payload MQTT

Cada dispositivo publica en su tópico un JSON con la siguiente forma:

```json
{
  "temperature":   { "value": 23.5, "unit": "°C"  },
  "humidity":      { "value": 60.1, "unit": "%"   },
  "soil_humidity": { "value": 42.0, "unit": "%"   },
  "light":         { "value": 320,  "unit": "lux" },
  "pressure":      { "value": 1013, "unit": "hPa" }
}
```

También se aceptan valores escalares: `{ "temperature": 23.5 }`.

### Tópicos

| Dispositivo | Tópico MQTT |
|---|---|
| ESP8266 | `sensors/esp8266` |
| Raspberry Pi Sense HAT | `sensors/raspberrypi` |
| (cualquier futuro sensor) | `sensors/<nombre>` |

---

## Configuración del ESP8266

El ESP8266 ya está transmitiendo datos por MQTT. Solo asegúrate de que:

1. El broker apunta a la IP de la Raspberry Pi.
2. El payload publicado sigue el formato JSON descrito arriba.
3. El tópico de publicación es `sensors/esp8266`.

Ejemplo de código Arduino/MicroPython mínimo:

```cpp
// Arduino (PubSubClient)
String payload = "{\"temperature\":{\"value\":" + String(temp, 1) +
                 ",\"unit\":\"°C\"},\"humidity\":{\"value\":" + String(hum, 1) +
                 ",\"unit\":\"%\"},\"soil_humidity\":{\"value\":" + String(soil, 0) +
                 ",\"unit\":\"%\"},\"light\":{\"value\":" + String(lux, 0) +
                 ",\"unit\":\"lux\"}}";
client.publish("sensors/esp8266", payload.c_str());
```

---

## API REST

| Endpoint | Descripción |
|---|---|
| `GET /api/latest` | Última lectura de cada sensor |
| `GET /api/history?source=&field=&limit=` | Historial filtrable |
| `GET /api/sources` | Lista de fuentes (dispositivos) activos |

---

## Tests

```bash
source venv/bin/activate
python -m pytest tests/ -v
```

---

## Servicios systemd

```bash
# Estado
systemctl status em-mqtt-client em-sensehat-client em-web-dashboard

# Reiniciar
sudo systemctl restart em-mqtt-client

# Logs
journalctl -u em-mqtt-client -f
```
