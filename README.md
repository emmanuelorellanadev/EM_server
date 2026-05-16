# EM Server – Monitor Meteorológico para Invernadero

Sistema de monitoreo IoT que **recibe, almacena y visualiza** datos climáticos de un invernadero.
Integra dos fuentes de datos:

| Dispositivo | Sensores | Protocolo |
|---|---|---|
| **ESP8266 NodeMCU V3** | Humedad de suelo (K8/C11), estado de riego | MQTT |
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
│                               │  mqtt_client.py  │   │
│                               │  (suscriptor)    │   │
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
│                               │  app.py  :5000   │   │
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
> en `sense_hat_client.py` según tu instalación.

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
updateWatering()        ← decide si activar/detener el riego
      │
      ▼
buildJson()             ← construye el payload MQTT
      │
      ▼
mqtt.publish()          ─────────────────────────────────────────►
                                                                   │
                                                          mqtt_client.py
                                                                   │
                                                          field_mapping()  ← "percent" → "soil_humidity"
                                                                   │
                                                          insert_readings_from_payload()
                                                                   │
                                                              SQLite DB
                                                                   │
                                                           Flask Dashboard
```

#### Payload MQTT del ESP8266

El ESP8266 publica en `sensors/esp8266` cada 60 s (configurable):

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
- `last_watered_sec` → se guarda y además se transforma a `last_watering_at_epoch` (timestamp UNIX) para mostrar la hora del último riego
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
├── requirements.txt         # Dependencias Python
├── setup.sh                 # Script de instalación automática (Raspbian)
│
├── database.py              # Capa de acceso a SQLite
├── mqtt_client.py           # Servicio suscriptor MQTT → DB (con field_mappings)
├── sense_hat_client.py      # Publicador de datos del Sense HAT → MQTT
├── app.py                   # Dashboard web Flask
│
├── esp8266/
│   └── humedadSueloK8/
│       ├── humedadSueloK8.ino   # Sketch Arduino completo
│       └── config.example.h     # Plantilla de configuración
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
│   ├── em-mqtt-client.service
│   ├── em-sensehat-client.service
│   └── em-web-dashboard.service
│
└── tests/
    ├── test_database.py
    ├── test_app.py
    └── test_mqtt_client.py
```

---

## Instalación rápida en Raspbian

```bash
# 1. Clona el repositorio
git clone https://github.com/emmanuelorellanadev/EM_server.git /home/pi/EM_server
cd /home/pi/EM_server

# 2. (Opcional) Ajusta config.json con la IP del broker y credenciales

# 3. Ejecuta el script de instalación como root
sudo bash setup.sh
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
python mqtt_client.py --config config.json

# Publicador Sense HAT
python sense_hat_client.py --config config.json

# Dashboard web
python app.py --config config.json
```

Abre `http://<IP_de_la_Raspberry>:5000` en el navegador.

---

## Configurar el ESP8266

```bash
# 1. Copia la plantilla de configuración
cp esp8266/humedadSueloK8/config.example.h \
   esp8266/humedadSueloK8/config.h

# 2. Edita config.h con tu red Wi-Fi, IP del broker y calibración

# 3. En Arduino IDE:
#    Herramientas → Placa → "NodeMCU 1.0 (ESP-12E Module)"
#    Herramientas → Puerto → el COM/ttyUSB del ESP8266
#    Instala "PubSubClient" de Nick O'Leary (Library Manager)
#    Compila y sube
```

El ESP8266 publicará automáticamente en `sensors/esp8266` cada 60 s
(ajustable con `BACKGROUND_SAMPLE_MS` en `config.h`).

Además, desde esta versión:
- publica su estado de presencia en `devices/esp8266/status` (`online`/`offline`, retain),
- usa LWT para marcar `offline` en caídas inesperadas,
- y envía una publicación inmediata al reconectar (no espera al siguiente intervalo).

---

## Configuración (config.json)

| Clave | Descripción | Default |
|---|---|---|
| `mqtt.broker` | Dirección del broker MQTT | `localhost` |
| `mqtt.port` | Puerto del broker | `1883` |
| `mqtt.username` / `password` | Credenciales (vacío = sin auth) | `""` |
| `mqtt.topics.device_status` | Tópico wildcard de presencia de dispositivos | `devices/+/status` |
| `database.path` | Ruta del archivo SQLite | `em_server.db` |
| `web.port` | Puerto del dashboard Flask | `5000` |
| `sense_hat.publish_interval_seconds` | Intervalo del Sense HAT | `30` |
| `field_mappings` | Mapeo de nombres de campo por fuente | ver abajo |

### field_mappings

Permite renombrar campos del payload MQTT antes de guardarlos en la base de datos.
Útil para normalizar payloads de dispositivos de terceros:

```json
"field_mappings": {
  "esp8266": {
    "percent": "soil_humidity",
    "raw":     "soil_raw"
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
