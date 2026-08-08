# EM Server – Monitor Meteorológico para Invernadero

Sistema de monitoreo IoT que **recibe, almacena y visualiza** datos climáticos de un invernadero.
Integra dos fuentes de datos:

| Dispositivo | Sensores | Protocolo |
|---|---|---|
| **ESP8266 NodeMCU V3** | Humedad de suelo (K8/C11), estado de riego | MQTT |
| **ESP32** | Humedad de suelo (K8/C11), temperatura/humedad DHT, luz LDR, estado de riego | MQTT |
| **Raspberry Pi + Sense HAT v1** | Temperatura, humedad ambiental, presión | MQTT (local) |

---

## Cómo funciona el sistema (Hardware + Software)

### 1. Diagrama general

```
┌──────────────────────────────────────────────────────┐
│                    INVERNADERO                        │
│                                                       │
│  ┌────────────────────────┐                          │
│  │    ESP8266 NodeMCU V3  │                          │
│  │                        │                          │
│  │  A0 ←── Sensor K8/C11 │  (humedad de suelo)      │
│  │  D5 ←── Salida digital │  (umbral seco/mojado)    │
│  │  D6 ──► Relé 5 V ──►  │                          │
│  │         Electroválvula │  (riego automático)      │
│  └────────┬───────────────┘                          │
│           │ Wi-Fi                                     │
└───────────┼──────────────────────────────────────────┘
            │ MQTT  sensors/esp8266
            ▼
┌──────────────────────────────────────────────────────┐
│              RASPBERRY PI (servidor)                  │
│                                                       │
│  ┌──────────────┐   MQTT       ┌──────────────────┐  │
│  │ Sense HAT v1 │─────────────►│  Mosquitto Broker│  │
│  │ temp/hum/pres│  local       │  (localhost:1883) │  │
│  └──────────────┘              └────────┬─────────┘  │
│                                         │             │
│                            subscribe (sensors/#)      │
│                                         ▼             │
│                               ┌──────────────────┐   │
│                               │ mqtt_service.py  │   │
│                               │ (suscriptor)     │   │
│                               └────────┬─────────┘   │
│                                        │ INSERT       │
│                                        ▼             │
│                               ┌──────────────────┐   │
│                               │  SQLite DB        │   │
│                               │  em_server.db     │   │
│                               └────────┬─────────┘   │
│                                        │ SELECT       │
│                                        ▼             │
│                               ┌──────────────────┐   │
│                               │  Flask Dashboard  │   │
│                               │  em_server :8080  │   │
│                               └──────────────────┘   │
└──────────────────────────────────────────────────────┘
```

---

### 2. Hardware

#### Raspberry Pi + Sense HAT v1

La Raspberry Pi actúa como **servidor central**. El Sense HAT v1 mide:

| Sensor | Campo en DB | Unidad |
|---|---|---|
| Temperatura (HTS221) | `temperature` | °C |
| Humedad relativa (HTS221) | `humidity` | % |
| Presión barométrica (LPS25H) | `pressure` | hPa |

> **Nota:** El sensor de temperatura del Sense HAT puede leer ~5 °C por encima
> de la temperatura real debido al calor de la CPU.  Ajusta `CPU_TEMP_CORRECTION`
> en `em_server/services/sensehat_service.py` según tu instalación.

#### ESP8266 NodeMCU V3 (invernadero)

El ESP8266 lee el suelo del invernadero y controla el riego:

```
ESP8266 NodeMCU V3
┌─────────────────────────────────┐
│  3V3 ──► VCC del sensor K8     │
│  GND ──► GND del sensor K8     │
│  A0  ◄── AO  del sensor K8     │  lectura analógica 0-1023
│  D5  ◄── DO  del sensor K8     │  umbral digital HIGH/LOW
│  D6  ──► IN  del módulo relé   │  GPIO12 — control del riego
│  VIN ──► 5V externo (o USB)    │
└─────────────────────────────────┘

Módulo relé 5 V
┌──────────────────────────────────┐
│  IN  ◄── D6 del ESP8266         │
│  VCC ◄── 5V externo             │
│  GND ◄── GND común              │
│  NO  ──► (+) Electroválvula 12V │  Contacto Normalmente Abierto
│  COM ──► (+) Fuente 12V         │
└──────────────────────────────────┘
                │
                └──► Diodo flyback (1N4007) en paralelo con la bobina
                     de la electroválvula para proteger el relé
```

| Pin ESP8266 | GPIO | Función |
|---|---|---|
| A0 | ADC0 | Lectura analógica del sensor K8 (0-1023) |
| D5 | GPIO14 | Salida digital del sensor K8 (umbral) |
| D6 | GPIO12 | Control del relé (HIGH = riego ON) |

#### Sensor K8 / C11 (humedad de suelo)

El sensor K8/C11 es de tipo **resistivo/capacitivo** y tiene dos salidas:
- **AO (analógica):** tensión proporcional a la resistencia del suelo.
  - Suelo **seco** → resistencia alta → tensión alta → ADC ≈ 1023
  - Suelo **húmedo** → resistencia baja → tensión baja → ADC ≈ 300
- **DO (digital):** HIGH/LOW según el potenciómetro de calibración del módulo.

El firmware convierte el ADC crudo a porcentaje de humedad:
```
humedad_% = (RAW_DRY - raw) / (RAW_DRY - RAW_WET) × 100
```
Ajusta `RAW_DRY` y `RAW_WET` en `config.h` con mediciones reales.

---

### 3. Software

#### Flujo de datos

```
Lectura del sensor
      │
      ▼
rawToPercent()          ← convierte ADC crudo a %
      │
      ▼
updateRelay()           ← decide estado del rele
      │
      ▼
publicarMQTT()          ← publica payload agregado
      │
      ▼
mqtt.publish()          ─────────────────────────────────────────►
                                                                    │
                                                           mqtt_service.py
                                                                    │
                                                           field mappings    ← normaliza campos por fuente
                                                                   │
                                                          insert_readings_from_payload()
                                                                   │
                                                              SQLite DB
                                                                   │
                                                           Flask Dashboard
```

#### Payload MQTT del ESP8266 (histórico)

> **Nota:** El firmware del ESP8266 (`firmware/esp8266/`) fue reemplazado por el firmware del ESP32 (`firmware/esp32/esp32.ino`).
> Esta sección se conserva como referencia para quienes aún usen el ESP8266 original.

El ESP publica en `sensors/<nodo>` en ventanas de agregacion (configurable):

```json
{
  "raw":      512,      ← lectura ADC cruda (0-1023)
  "percent":  42.3,     ← humedad de suelo (0% = seco, 100% = mojado)
  "state":    "WET",    ← "DRY" | "WET" | "WATERING" | "COOLDOWN"
  "watering": false,    ← ¿está el riego activo?
  "cooldown": false,    ← ¿en período de espera post-riego?
  "last_watered_sec": 120 ← segundos desde el último riego (-1 = nunca)
}
```

El servidor aplica el mapeo configurado en `config.json`:
- `percent` → se guarda como campo `soil_humidity`
- `raw` → se guarda como campo `soil_raw`
- `watering` y `cooldown` (booleanos) → se guardan como `1.0` / `0.0`
- `last_watered_sec` → se guarda y además se transforma a `last_watering_at_epoch` (timestamp UNIX; `-1` cuando no hay riego registrado) para mostrar la hora del último riego
- `state` (cadena de texto) → se ignora en la base de datos

#### Payload MQTT del ESP32

El ESP32 publica en `sensors/<nodo>` con ventana de agregacion (configurable).
A diferencia del ESP8266, incluye sensores ambiental (DHT) y de luz (LDR):

```json
{
  "soil_vwc":              42.3,     ← humedad de suelo (mapeado a soil_humidity)
  "watering":             false,    ← ¿esta el riego activo?
  "state":                "WET",    ← "DRY" | "WET" | "WATERING" | "COOLDOWN"
  "last_watered_sec":     120,      ← segundos desde el ultimo riego
  "on_threshold_soil_vwc": 35,      ← umbral de activacion del riego
  "relay_on_time_s":      5.0,       ← duracion del riego en segundos
  "temperature":          25.3,     ← temperatura ambiental (DHT)
  "humidity":             60.1,     ← humedad ambiental (DHT)
  "light_raw":            2048,     ← valor ADC crudo del LDR (0-4095)
  "light_percent":        50.0      ← luz ambiental convertida a porcentaje
}
```

El servidor aplica el mapeo configurado en `config.json`:
- `soil_vwc` → se guarda como campo `soil_humidity`
- `temperature`, `humidity`, `light_raw`, `light_percent` → se guardan con su nombre original
- `watering` (booleano) → se guarda como `1.0` / `0.0`
- `last_watered_sec` → ademas se transforma a `last_watering_at_epoch`
- `state` (cadena de texto) → se ignora en la base de datos

#### Payload MQTT del Sense HAT

El Sense HAT publica en `sensors/raspberrypi` cada 30 s:

```json
{
  "temperature": { "value": 22.5, "unit": "°C"  },
  "humidity":    { "value": 58.2, "unit": "%"   },
  "pressure":    { "value": 1013, "unit": "hPa" }
}
```

#### Lógica de riego automático (ESP8266)

```
humedad < UMBRAL_RIEGO (30%)  AND  !cooldown
           │
           ▼
     Abrir electroválvula (relé HIGH)
           │
           ├─ humedad >= UMBRAL_CORTE (60%)
           │          O
           └─ tiempo >= DURACION_RIEGO_MS (10 s)
                        │
                        ▼
               Cerrar electroválvula
               Iniciar cooldown (5 min)
```

---

## Estructura del proyecto

```
EM_server/
├── config.json              # Configuración central (MQTT, DB, Web, field_mappings)
├── requirements.txt         # Dependencias Python de producción
├── requirements-dev.txt     # Dependencias de desarrollo (pytest, ruff)
├── pyproject.toml           # Configuración del paquete Python
│
├── em_server/               # Paquete principal de Python
│   ├── __main__.py          # Punto de entrada: python -m em_server
│   ├── app.py               # Factory de la app Flask (create_app)
│   ├── config.py            # Carga centralizada de config.json
│   ├── models/
│   │   └── database.py      # Capa de acceso a SQLite
│   ├── services/
│   │   ├── mqtt_service.py      # Suscriptor MQTT → DB (con field_mappings)
│   │   └── sensehat_service.py  # Publicador de datos del Sense HAT → MQTT
│   ├── routes/
│   │   ├── dashboard.py     # Rutas HTML (/, /history)
│   │   └── api.py           # Rutas JSON (/api/*) y comando de riego
│   └── utils/
│       ├── log_config.py    # Configuración de logging
│       └── formatters.py    # Formateo de datos y metadata de campos
│
├── firmware/                # Código Arduino (microcontroladores)
│   ├── esp32/
│   │   ├── esp32.ino            # Sketch Arduino completo
│   │   ├── config.example.h     # Plantilla de configuración
│   │   └── CONFIGURACION.md     # Guía de configuración
│   └── esp8266/
│       ├── humedadSueloK8.ino
│       ├── config.example.h
│       └── CONFIGURACION.md
│
├── deploy/
│   ├── setup.sh             # Script de instalación automática (Raspbian)
│   └── systemd/
│       ├── em-mqtt-client.service
│       ├── em-sensehat-client.service
│       └── em-web-dashboard.service
│
├── templates/
│   ├── base.html            # Plantilla base
│   ├── index.html           # Dashboard con lecturas actuales y gráficas
│   └── history.html         # Tabla de historial con filtros
│
├── static/
│   ├── css/style.css        # Estilos del dashboard
│   └── js/dashboard.js      # Gráficas (Chart.js) y refresco manual
│
└── tests/
    ├── test_database.py
    ├── test_app.py
    ├── test_mqtt_client.py
    └── test_sense_hat_client.py
```

---

## Instalación rápida en Raspbian

```bash
# 1. Clona el repositorio
git clone https://github.com/emmanuelorellanadev/EM_server.git /home/pi/EM_server
cd /home/pi/EM_server

# 2. (Opcional) Ajusta config.json con la IP del broker y credenciales

# 3. Ejecuta el script de instalación como root
sudo bash deploy/setup.sh
```

El script:
1. Instala Mosquitto, Python 3 y python3-sense-hat
2. Crea un entorno virtual Python con todas las dependencias
3. Genera una clave secreta Flask aleatoria
4. Habilita los tres servicios systemd para arranque automático

---

## Instalación manual

### 1. Sistema

```bash
sudo apt-get update
sudo apt-get install -y mosquitto mosquitto-clients python3 python3-venv python3-sense-hat
sudo systemctl enable --now mosquitto
```

### 2. Python

```bash
python3 -m venv --system-site-packages venv
source venv/bin/activate
pip install -r requirements.txt
```

### 3. Iniciar servicios

```bash
# Suscriptor MQTT (almacena en SQLite)
python -m em_server.services.mqtt_service --config config.json

# Publicador Sense HAT
python -m em_server.services.sensehat_service --config config.json

# Dashboard web
python -m em_server --config config.json
```

Abre `http://<IP_de_la_Raspberry>:8080` en el navegador.

---

## Configurar el ESP32

```bash
# 1. Copia la plantilla de configuración
cp firmware/esp32/config.example.h \
   firmware/esp32/config.h

# 2. Edita config.h con tu red Wi-Fi, IP del broker y calibración

# 3. En Arduino IDE:
#    Herramientas → Placa → "ESP32 Dev Module" o "NodeMCU-32S"
#    Herramientas → Puerto → el COM/ttyUSB del ESP32
#    Instala "PubSubClient" de Nick O'Leary y "DHT sensor library" de Adafruit
#    Compila y sube
```

El ESP publica telemetria agregada segun:
- `AGGREGATION_SAMPLE_MS`
- `MQTT_PUBLISH_INTERVAL_MS`
- `MQTT_WINDOW_SAMPLE_COUNT`

El nodo publica presencia en `devices/<nodo>/status` (`online`/`offline`) con LWT.

---

## Configuración (config.json)

| Clave | Descripción | Default |
|---|---|---|
| `mqtt.broker` | Dirección del broker MQTT | `localhost` |
| `mqtt.port` | Puerto del broker | `1883` |
| `mqtt.username` / `password` | Credenciales (vacío = sin auth) | `""` |
| `mqtt.client_id_subscriber` | Client ID del suscriptor MQTT | `em_server_subscriber` |
| `mqtt.client_id_sensehat` | Client ID del publicador Sense HAT | `em_server_sensehat` |
| `mqtt.topics.all` | Tópico wildcard de suscripción | `sensors/#` |
| `mqtt.topics.device_status` | Tópico wildcard de presencia de dispositivos | `devices/+/status` |
| `database.path` | Ruta del archivo SQLite | `em_db/em_server.db` |
| `web.host` | Interfaz de red del servidor Flask | `0.0.0.0` |
| `web.port` | Puerto del dashboard Flask | `8080` |
| `web.debug` | Modo debug de Flask | `false` |
| `web.secret_key` | Clave secreta para sesiones Flask | `change-this-secret-key-in-production` |
| `sense_hat.topic` | Tópico MQTT del Sense HAT | `sensors/raspberrypi` |
| `sense_hat.publish_interval_seconds` | Intervalo de publicación del Sense HAT | `30` |
| `sense_hat.cpu_temp_correction` | Corrección por calor de CPU (°C) | `5.0` |
| `field_mappings` | Mapeo de nombres de campo por fuente | ver abajo |

### field_mappings

Permite renombrar campos del payload MQTT antes de guardarlos en la base de datos.
Útil para normalizar payloads de dispositivos de terceros:

```json
"field_mappings": {
  "esp8266": {
    "percent": "soil_humidity"
  },
  "esp32_01": {
    "soil_vwc": "soil_humidity"
  },
  "esp32_02": {
    "soil_vwc": "soil_humidity",
    "percent": "soil_humidity"
  }
}
```

---

## API REST

| Endpoint | Descripción |
|---|---|
| `GET /api/latest` | Última lectura de cada sensor |
| `GET /api/history?source=&field=&limit=` | Historial filtrable |
| `GET /api/sources` | Lista de fuentes activas |
| `GET /api/trend?source=&range=` | Datos para grafica de tendencia (`1h`, `1d`, `1w`, `1m`, `1y`) |
| `POST /api/command/water` | Envia comando de riego manual por MQTT |

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

# Logs en vivo
journalctl -u em-mqtt-client -f
```
