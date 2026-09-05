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
│  │ temp/hum/pres│  local       │  (localhost:8883) │  │
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

> Para la fórmula de conversión ADC a porcentaje y las variables de calibración
> `RAW_DRY`/`RAW_WET`, ver `firmware/esp32/CONFIGURACION.md` sección 2.

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
├── config.example.json        # Plantilla de configuración (segura para git)
├── config.json                # Configuración local (NO se sube al repo)
├── requirements.txt           # Dependencias Python de producción
├── requirements-dev.txt       # Dependencias de desarrollo (pytest, ruff)
├── pyproject.toml             # Configuración del paquete Python
│
├── em_server/               # Paquete principal de Python
│   ├── __main__.py          # Punto de entrada: python -m em_server
│   ├── app.py               # Factory de la app Flask (create_app)
│   ├── config.py            # Carga centralizada de config.json (con env vars)
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
│   ├── mosquitto/
│   │   ├── tls.conf         # Configuración TLS/mTLS del broker
│   │   └── acl              # Control de acceso por topic y dispositivo
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

# 2. (Opcional) Crea config.json desde la plantilla y edítalo
cp config.example.json config.json
# Editar config.json con la IP del broker y credenciales

# 3. Ejecuta el script de instalación como root
sudo bash deploy/setup.sh
```

El script:
1. Instala Mosquitto, Python 3 y python3-sense-hat
2. Crea un entorno virtual Python con todas las dependencias
3. Copia `config.example.json` → `config.json` si no existe y genera una clave secreta
4. Habilita los tres servicios systemd para arranque automático

#### Configurar TLS (recomendado para producción)

```bash
# 1. Copiar certificados a la Raspberry Pi
scp certs/ca.crt certs/EM-broker.crt certs/EM-broker.key pi@raspberry:/tmp/
scp certs/ca.crt certs/EM-mqtt_client.crt certs/EM-mqtt_client.key pi@raspberry:/tmp/em_certs/
scp firmware/esp32/certs/EM-esp32_01.crt firmware/esp32/certs/EM-esp32_01.key pi@raspberry:/tmp/esp32_certs/

# 2. En la Raspberry Pi, instalar certificados
ssh pi@raspberry

# Certificados del broker
sudo mkdir -p /etc/mosquitto/certs
sudo cp /tmp/ca.crt /tmp/EM-broker.crt /tmp/EM-broker.key /etc/mosquitto/certs/
sudo chown mosquitto:mosquitto /etc/mosquitto/certs/*
sudo chmod 600 /etc/mosquitto/certs/EM-broker.key

# Certificados del cliente Python
sudo mkdir -p /etc/em/certs
sudo cp /tmp/em_certs/ca.crt /tmp/em_certs/EM-mqtt_client.crt /tmp/em_certs/EM-mqtt_client.key /etc/em/certs/
sudo chmod 600 /etc/em/certs/EM-mqtt_client.key

# Certificados del ESP32 (para pruebas)
sudo mkdir -p /etc/em/esp32_certs
sudo cp /tmp/esp32_certs/EM-esp32_01.crt /tmp/esp32_certs/EM-esp32_01.key /etc/em/esp32_certs/
sudo chmod 600 /etc/em/esp32_certs/EM-esp32_01.key

# 3. Copiar configuración TLS
sudo cp deploy/mosquitto/tls.conf /etc/mosquitto/conf.d/
sudo cp deploy/mosquitto/acl /etc/mosquitto/acl
sudo systemctl restart mosquitto
```

Ver [Seguridad MQTT](#seguridad-mqtt-tlsmtls) para más detalles.

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

### 3. Configurar

```bash
cp config.example.json config.json
# Editar config.json con la IP del broker y credenciales
```

### 4. Configurar TLS (recomendado)

```bash
# Copiar certificados del broker
sudo mkdir -p /etc/mosquitto/certs
sudo cp certs/ca.crt certs/EM-broker.crt certs/EM-broker.key /etc/mosquitto/certs/
sudo chown mosquitto:mosquitto /etc/mosquitto/certs/*
sudo chmod 600 /etc/mosquitto/certs/EM-broker.key

# Copiar certificados del cliente Python
sudo mkdir -p /etc/em/certs
sudo cp certs/ca.crt certs/EM-mqtt_client.crt certs/EM-mqtt_client.key /etc/em/certs/
sudo chmod 600 /etc/em/certs/EM-mqtt_client.key

# Copiar certificados del ESP32 (para pruebas)
sudo mkdir -p /etc/em/esp32_certs
sudo cp firmware/esp32/certs/EM-esp32_01.crt firmware/esp32/certs/EM-esp32_01.key /etc/em/esp32_certs/
sudo chmod 600 /etc/em/esp32_certs/EM-esp32_01.key

# Configurar Mosquitto
sudo cp deploy/mosquitto/tls.conf /etc/mosquitto/conf.d/
sudo cp deploy/mosquitto/acl /etc/mosquitto/acl
sudo systemctl restart mosquitto
```

### 5. Iniciar servicios

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
cp firmware/esp32/config.example.h firmware/esp32/config.h

# 2. Edita config.h con tu red Wi-Fi, IP del broker y calibración

# 3. En Arduino IDE:
#    Herramientas → Placa → "ESP32 Dev Module" o "NodeMCU-32S"
#    Herramientas → Puerto → el COM/ttyUSB del ESP32
#    Instala "PubSubClient" de Nick O'Leary y "DHT sensor library" de Adafruit
#    Compila y sube
```

Cableado del ESP32 (DevKit):

| Señal | GPIO | Nota |
|---|---|---|
| AO sensor de suelo (K8/C11) | 34 (D34) | ADC1_CH6, lectura analógica |
| DO sensor de suelo (opcional) | 36 | solo informativo |
| LDR | 35 (D35) | ADC1_CH7, lectura analógica |
| DHT22 (temp./humedad) | 27 (D27) | lectura digital |
| Relé (active-high) | 12 (D12) | salida digital |

> Para calibración ADC, temporizadores de muestreo, umbrales de riego,
> modo debug y configuración TLS/mTLS del ESP32, ver
> `firmware/esp32/CONFIGURACION.md`.

El nodo publica presencia en `devices/<nodo>/status` (`online`/`offline`) con LWT.

---

## Configuración

La configuración se carga desde `config.json`. Los valores pueden ser
sobreescritos por variables de entorno (útil en producción):

| Variable de entorno | Sección en JSON | Descripción |
|---|---|---|
| `EM_MQTT_BROKER` | `mqtt.broker` | Dirección del broker MQTT |
| `EM_MQTT_PORT` | `mqtt.port` | Puerto del broker |
| `EM_MQTT_USERNAME` | `mqtt.username` | Usuario MQTT |
| `EM_MQTT_PASSWORD` | `mqtt.password` | Contraseña MQTT |
| `EM_SECRET_KEY` | `web.secret_key` | Clave secreta Flask |
| `EM_WEB_HOST` | `web.host` | Interfaz de red |
| `EM_WEB_PORT` | `web.port` | Puerto del dashboard |
| `EM_WEB_DEBUG` | `web.debug` | Modo debug |
| `EM_DB_PATH` | `database.path` | Ruta de SQLite |
| `EM_API_KEY` | `web.api_key` | API key para autenticación |
| `EM_MQTT_TLS_ENABLED` | `mqtt.tls.enabled` | Habilitar TLS (`true`/`false`) |
| `EM_MQTT_TLS_CA_CERT` | `mqtt.tls.ca_cert` | Ruta a `ca.crt` |
| `EM_MQTT_TLS_CLIENT_CERT` | `mqtt.tls.client_cert` | Ruta a certificado de cliente (`EM-mqtt_client.crt`) |
| `EM_MQTT_TLS_CLIENT_KEY` | `mqtt.tls.client_key` | Ruta a clave privada de cliente (`EM-mqtt_client.key`) |

### Parámetros de config.json

| Clave | Descripción | Default |
|---|---|---|
| `mqtt.broker` | Dirección del broker MQTT | `localhost` |
| `mqtt.port` | Puerto del broker | `1883` |
| `mqtt.username` / `password` | Credenciales (vacío = sin auth) | `""` |
| `mqtt.client_id_subscriber` | Client ID del suscriptor MQTT | `em_server_subscriber` |
| `mqtt.client_id_sensehat` | Client ID del publicador Sense HAT | `em_server_sensehat` |
| `mqtt.tls.enabled` | Habilitar TLS/mTLS | `false` |
| `mqtt.tls.ca_cert` | Ruta al certificado CA (`ca.crt`) | `""` |
| `mqtt.tls.client_cert` | Ruta al certificado del cliente | `""` |
| `mqtt.tls.client_key` | Ruta a la clave privada del cliente | `""` |
| `mqtt.tls.insecure` | Saltar verificación de hostname (solo desarrollo) | `false` |
| `mqtt.topics.all` | Tópico wildcard de suscripción | `sensors/#` |
| `mqtt.topics.device_status` | Tópico wildcard de presencia de dispositivos | `devices/+/status` |
| `database.path` | Ruta del archivo SQLite | `em_db/em_server.db` |
| `web.host` | Interfaz de red del servidor Flask | `0.0.0.0` |
| `web.port` | Puerto del dashboard Flask | `8080` |
| `web.debug` | Modo debug de Flask | `false` |
| `web.secret_key` | Clave secreta para sesiones Flask | `CHANGE_THIS_IN_PRODUCTION` |
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

## Seguridad MQTT (TLS/mTLS)

### ¿Qué es mTLS?

**TLS** (Transport Layer Security) cifra la comunicación entre cliente y servidor.
**mTLS** (mutual TLS) va más allá: ambos lados se autentican mutuamente con certificados.

```
TLS normal:   Cliente verifica al Broker
mTLS:         Cliente verifica al Broker  Y  Broker verifica al Cliente
```

En EM_server, mTLS garantiza que:
- El ESP32 solo se conecta a **tu** Raspberry Pi (no a un broker falso)
- El broker solo acepta conexiones de **tus** dispositivos autorizados
- Todo el tráfico MQTT viaja cifrado (sin posibilidad de sniffing)

### Paso 1: Crear la CA (Autoridad Certificadora)

La CA es la raíz de confianza. Con ella firmas todos los certificados del sistema.

> **IMPORTANTE:** `ca.key` es el archivo más crítico. Si se compromete, toda la
> cadena de confianza se rompe. Guárdalo en un USB offline y NUNCA lo subas al repo.

```bash
mkdir -p certs && cd certs

# Generar clave privada de la CA (4096 bits para máxima seguridad)
openssl genrsa -out ca.key 4096

# Crear certificado autofirmado de la CA (válido 10 años)
openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 \
  -out ca.crt -subj "/CN=EM-Root-CA"
```

### Paso 2: Crear certificado del broker Mosquitto

El broker necesita un certificado con **SANs** (Subject Alternative Names) para
que los clientes puedan conectarse por diferentes hostnames/IPs.

```bash
# 1. Crear archivo de extensión con SANs
cat > broker_ext.cnf << 'EOF'
[req]
distinguished_name = req_distinguished_name
req_extensions = v3_req

[req_distinguished_name]

[v3_req]
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
DNS.1 = localhost
DNS.2 = raspberrypi.local
IP.1 = 127.0.0.1
IP.2 = 192.168.1.2
EOF

# 2. Generar clave privada del broker
openssl genrsa -out EM-broker.key 2048

# 3. Crear CSR (Certificate Signing Request)
openssl req -new -key EM-broker.key -out EM-broker.csr \
  -subj "/CN=raspberrypi.local" -config broker_ext.cnf

# 4. Firmar con la CA (incluyendo SANs)
openssl x509 -req -in EM-broker.csr \
  -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out EM-broker.crt -days 825 -sha256 \
  -extensions v3_req -extfile broker_ext.cnf

# 5. Verificar SANs
openssl x509 -in EM-broker.crt -noout -text | grep -A5 "Subject Alternative"
# Debe mostrar: DNS:localhost, DNS:raspberrypi.local, IP:127.0.0.1, IP:192.168.1.2

# 6. Limpiar archivos temporales
rm -f EM-broker.csr broker_ext.cnf
```

> **¿Por qué SANs?** OpenSSL moderno ya NO acepta solo el CN para validar
> hostname. Necesitas SANs para que `localhost`, `raspberrypi.local` e IP
> sean aceptados como nombres válidos del servidor.

### Paso 3: Crear certificado del cliente ESP32

```bash
# Generar clave privada del ESP32
openssl genrsa -out EM-esp32_01.key 2048

# Crear CSR con CN=EM-esp32_01
openssl req -new -key EM-esp32_01.key -out EM-esp32_01.csr \
  -subj "/CN=EM-esp32_01"

# Firmar con la CA
openssl x509 -req -in EM-esp32_01.csr \
  -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out EM-esp32_01.crt -days 825 -sha256

# Limpiar
rm -f EM-esp32_01.csr
```

### Paso 4: Crear certificado del cliente Python

```bash
# Generar clave privada del suscriptor Python
openssl genrsa -out EM-mqtt_client.key 2048

# Crear CSR con CN=em_server_subscriber
openssl req -new -key EM-mqtt_client.key -out EM-mqtt_client.csr \
  -subj "/CN=em_server_subscriber"

# Firmar con la CA
openssl x509 -req -in EM-mqtt_client.csr \
  -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out EM-mqtt_client.crt -days 825 -sha256

# Limpiar
rm -f EM-mqtt_client.csr
```

### Estructura final de certificados

```
certs/                              ← Certificados del SERVIDOR (Python + Mosquitto)
├── ca.crt                          ← CA raiz (firma todos los certificados)
├── ca.key                          ← Clave privada de la CA (GUARDAR EN USB OFFLINE)
├── ca.srl                          ← Serial de la CA (auto-generado)
├── EM-broker.crt                   ← Certificado del broker (con SANs)
├── EM-broker.key                   ← Clave privada del broker
├── EM-mqtt_client.crt              ← Certificado del suscriptor Python (CN=em_server_subscriber)
└── EM-mqtt_client.key              ← Clave privada del suscriptor Python

firmware/esp32/certs/               ← Certificados del ESP32 (respaldo PEM)
├── ca.crt                          ← Misma CA raiz
├── EM-esp32_01.crt                 ← Certificado de identidad del ESP32 (CN=EM-esp32_01)
└── EM-esp32_01.key                 ← Clave privada del ESP32

firmware/esp32/certs.h              ← PEM embebidos en código C (NO subir al repo)
```

### Roles por certificado

| Certificado | Quién lo usa | Para qué |
|---|---|---|
| `ca.crt` | Todos | Raíz de confianza — valida que los certificados fueron firmados por tu CA |
| `EM-broker.crt`/`.key` | Mosquitto | Identidad del broker — el ESP32 y Python validan que hablan con el broker legítimo |
| `EM-esp32_01.crt`/`.key` | ESP32 | Identidad del ESP32 — el broker verifica que el cliente es `esp32_01` autorizado |
| `EM-mqtt_client.crt`/`.key` | Python | Identidad del suscriptor — el broker verifica que es `em_server_subscriber` |

### Paso 5: Desplegar en la Raspberry Pi

Copia los certificados desde tu Mac a la Pi. Ejecuta **en tu Mac**:

```bash
# Copiar certificados del broker
scp certs/ca.crt certs/EM-broker.crt certs/EM-broker.key pi@raspberry:/tmp/

# Copiar certificados del cliente Python
scp certs/ca.crt certs/EM-mqtt_client.crt certs/EM-mqtt_client.key pi@raspberry:/tmp/em_certs/

# Copiar certificados del ESP32 (para pruebas con mosquitto_pub)
scp firmware/esp32/certs/EM-esp32_01.crt firmware/esp32/certs/EM-esp32_01.key pi@raspberry:/tmp/esp32_certs/

# Copiar configuración TLS y ACL
scp deploy/mosquitto/tls.conf deploy/mosquitto/acl pi@raspberry:/tmp/
```

Ahora ejecuta **en la Raspberry Pi**:

```bash
# 1. Instalar certificados del broker
sudo mkdir -p /etc/mosquitto/certs
sudo cp /tmp/ca.crt /tmp/EM-broker.crt /tmp/EM-broker.key /etc/mosquitto/certs/
sudo chown mosquitto:mosquitto /etc/mosquitto/certs/*
sudo chmod 600 /etc/mosquitto/certs/EM-broker.key
sudo chmod 644 /etc/mosquitto/certs/ca.crt /etc/mosquitto/certs/EM-broker.crt

# 2. Instalar configuración TLS y ACL
sudo cp /tmp/tls.conf /etc/mosquitto/conf.d/
sudo cp /tmp/acl /etc/mosquitto/acl

# 3. Instalar certificados del cliente Python
sudo mkdir -p /etc/em/certs
sudo cp /tmp/em_certs/ca.crt /tmp/em_certs/EM-mqtt_client.crt /tmp/em_certs/EM-mqtt_client.key /etc/em/certs/
sudo chmod 600 /etc/em/certs/EM-mqtt_client.key
sudo chmod 644 /etc/em/certs/ca.crt /etc/em/certs/EM-mqtt_client.crt

# 4. Preparar certificados del ESP32 (para pruebas)
sudo mkdir -p /etc/em/esp32_certs
sudo cp /tmp/esp32_certs/EM-esp32_01.crt /tmp/esp32_certs/EM-esp32_01.key /etc/em/esp32_certs/
sudo chmod 600 /etc/em/esp32_certs/EM-esp32_01.key

# 5. Reiniciar Mosquitto
sudo systemctl restart mosquitto

# 6. Verificar que escucha en 8883
ss -tlnp | grep mosquitto
```

### Paso 6: Verificar la conexión (desde la Raspberry Pi)

```bash
# ── Prueba 1: Con certificado ESP32 (debe funcionar) ──────────────
mosquitto_pub -h localhost -p 8883 \
  --cafile /etc/mosquitto/certs/ca.crt \
  --cert /etc/em/esp32_certs/EM-esp32_01.crt \
  --key /etc/em/esp32_certs/EM-esp32_01.key \
  -t "sensors/esp32_01" -m '{"test": true}' -d
# Salida esperada: Client null sending CONNECT → CONNACK (0) → PUBLISH → DISCONNECT

# ── Prueba 2: Con certificado Python (debe funcionar) ─────────────
mosquitto_pub -h localhost -p 8883 \
  --cafile /etc/mosquitto/certs/ca.crt \
  --cert /etc/em/certs/EM-mqtt_client.crt \
  --key /etc/em/certs/EM-mqtt_client.key \
  -t "sensors/test" -m '{"test": true}' -d
# Salida esperada: CONNACK (0)

# ── Prueba 3: SIN certificado (debe FALLAR) ───────────────────────
mosquitto_pub -h localhost -p 8883 \
  --cafile /etc/mosquitto/certs/ca.crt \
  -t "test" -m "should fail" -d
# Salida esperada: Error de conexión / Connection refused

# ── Prueba 4: Verificar cipher TLS ────────────────────────────────
openssl s_client -connect localhost:8883 \
  -CAfile /etc/mosquitto/certs/ca.crt \
  -cert /etc/em/esp32_certs/EM-esp32_01.crt \
  -key /etc/em/esp32_certs/EM-esp32_01.key \
  </dev/null 2>/dev/null | grep -E "Protocol|Cipher|Verify"
# Salida esperada: Protocol=TLSv1.2, Verify return code: 0 (ok)

# ── Prueba 5: Verificar que un cert fue firmado por la CA ─────────
openssl verify -CAfile /etc/mosquitto/certs/ca.crt /etc/em/certs/EM-mqtt_client.crt
# Salida: OK
```

### Configurar el servidor Python

Verificar que `config.json` tenga el bloque TLS correcto:

```json
{
  "mqtt": {
    "broker": "localhost",
    "port": 8883,
    "tls": {
      "enabled": true,
      "ca_cert": "/etc/em/certs/ca.crt",
      "client_cert": "/etc/em/certs/EM-mqtt_client.crt",
      "client_key": "/etc/em/certs/EM-mqtt_client.key",
      "insecure": false
    }
  }
}
```

### Configurar el ESP32 (mTLS)

1. Verificar `MQTT_PORT 8883` en `config.h`
2. Generar `firmware/esp32/certs.h` con los PEM embebidos (ver script en CONFIGURACION.md §9.4)
3. El firmware `esp32.ino` ya incluye `WiFiClientSecure`, NTP y carga de certificados

> Para instrucciones detalladas sobre `certs.h` (sintaxis C, `PROGMEM`,
> generación manual/automática), cambios en `esp32.ino` y diagnóstico de TLS,
> ver `firmware/esp32/CONFIGURACION.md` sección 9.

### Flujo de autenticación mTLS

```
ESP32_01                              Mosquitto Broker                      CA
   │                                       │                               │
   │ ──── ClientHello (propongo TLS 1.2) ──►                               │
   │ ◄─── ServerHello + EM-broker.crt ──────                               │
   │                                       │                               │
   │ ──── valida EM-broker.crt con ca.crt ─┘                               │
   │                                       │                               │
   │ ◄─── CertificateRequest ──────────────│                               │
   │ ──── EM-esp32_01.crt ────────────────►│                               │
   │ ──── prueba con EM-esp32_01.key ─────►│ ──── valida firma ──────────►│
   │                                       │ ◄─── confianza por ca.crt ───┘
   │                                       │                               │
   │ ◄─── Finished (TLS OK) ──────────────│                               │
   │ ──── Finished (TLS OK) ─────────────►│                               │
   │                                       │                               │
   │ ════ MQTT cifrado (CONNECT/PUBLISH) ════════════════════════════════ │
```

> Para más detalles teóricos, ver `SEGURIDAD_MQTT_TLS.md` y `seguridad.md`.

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
