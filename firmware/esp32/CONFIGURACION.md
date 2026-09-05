# Configuracion del firmware `esp32`

Este documento describe la funcion de cada variable en `config.h`.
Los comentarios en el codigo .ino se mantienen minimos; la explicacion detallada esta aqui.

## 1. Objetivo

`config.h` define:
- Parametros electricos (GPIO, calibracion ADC de cada sensor)
- Umbrales y tiempos de riego
- Temporizadores de muestreo y publicacion MQTT
- Topicos y credenciales MQTT

El firmware lee estas constantes en compilacion. Si cambias valores, debes recompilar y cargar el binario al ESP.

## 2. Variables por sensor

### Sensor de suelo (humedad)

| Variable | Funcion |
|---|---|
| `PIN_AO` | Entrada analogica del sensor (GPIO 34 = ADC1_CH6, D34). Debe ser ADC1: los pines ADC2 devuelven 0 con WiFi activo |
| `PIN_DO` | Entrada digital del sensor (GPIO 36; solo informativo; el control usa el valor analogico) |
| `RAW_DRY` / `RAW_WET` | Calibracion: raw en aire seco y en agua. ⚠ Recalibrar en el ESP32: usa ADC de 12 bits (0-4095), los valores de ejemplo son de 10 bits |
| `ANALOG_SAMPLES` / `ANALOG_DELAY_MS` | Promedio de N lecturas para estabilidad |

Formula: `% = (RAW_DRY - raw) / (RAW_DRY - RAW_WET) × 100`

### LED integrado

| Variable | Funcion |
|---|---|
| `PIN_LED` | GPIO del LED integrado (GPIO 2) |
| `LED_ACTIVE_LOW` | `0` = active-high (se enciende con HIGH), `1` = active-low (se enciende con LOW) |

### Sensor ambiental DHT (temperatura y humedad, opcional)

| Variable | Funcion |
|---|---|
| `ENABLE_AMBIENT_SENSOR` | 1 = habilitado, 0 = deshabilitado |
| `PIN_AMBIENT` | GPIO del DHT (GPIO 27, D27) |
| `AMBIENT_SENSOR_DHT22` | 1 = DHT22, 0 = DHT11 |
| `AMBIENT_SAMPLES` / `AMBIENT_DELAY_MS` | Promedio interno del DHT |
| `TEMP_OFFSET_C` / `HUM_OFFSET_PCT` | Correccion fina (ej: +1.0 si el sensor mide 1 grado de mas) |

### Sensor de luz LDR (opcional)

| Variable | Funcion |
|---|---|
| `ENABLE_LIGHT_SENSOR` | 1 = habilitado, 0 = deshabilitado |
| `PIN_LIGHT` | GPIO del LDR (GPIO 35 = ADC1_CH7, D35) |
| `LIGHT_DARK_RAW` / `LIGHT_BRIGHT_RAW` | Calibracion: raw a oscuras y con luz maxima |

El LDR se conecta como divisor de tension con resistencia de 10K a GND (pulldown).
A diferencia del sensor de suelo, **mas luz = mas voltaje = raw mas alto**.

Formula: `% = (raw - LIGHT_DARK_RAW) / (LIGHT_BRIGHT_RAW - LIGHT_DARK_RAW) × 100`

## 3. Control de riego

| Variable | Funcion |
|---|---|
| `PIN_RELAY` | GPIO del rele (GPIO 12, D12), active-high |
| `ON_THRESHOLD_SOIL_VWC` | Si VWC (Volumetric Water Content) < este valor → abre valvula |
| `OFF_THRESHOLD_SOIL_VWC` | Solo informativo (web + JSON). No controla el relé; el riego siempre dura `RELAY_ON_TIME_MS` |
| `RELAY_ON_TIME_MS` | Tiempo que permanece abierta la valvula por ciclo |
| `COOLDOWN_MS` | Espera minima antes de permitir otro ciclo |

Maquina de estados: `IDLE → WATERING → COOLDOWN → IDLE`

## 4. Muestreo y publicacion por ventana

El firmware usa **tres ritmos independientes**:

1. **`CONTROL_SAMPLE_MS`**  
   Lectura rapida del ADC para decidir riego local. NO afecta la ventana MQTT.

2. **`AGGREGATION_SAMPLE_MS`**  
   Intervalo entre muestras completas (suelo + DHT + LDR) que se guardan en un buffer circular.

3. **`MQTT_PUBLISH_INTERVAL_MS`**  
   Intervalo entre publicaciones MQTT del promedio de la ventana.

Relacion obligatoria (validada en compilacion):

```
MQTT_WINDOW_SAMPLE_COUNT = MQTT_PUBLISH_INTERVAL_MS / AGGREGATION_SAMPLE_MS
```

### Ejemplo: ventana de 6 muestras, publicacion cada 3 min

```
AGGREGATION_SAMPLE_MS    = 30000   (30 s entre muestras)
MQTT_PUBLISH_INTERVAL_MS = 180000  (3 min entre publicaciones)
MQTT_WINDOW_SAMPLE_COUNT = 6       (180000 / 30000)
```

El buffer acumula 6 muestras (una cada 30 s) y cada 3 min publica el promedio.

## 5. Modo configuración

| Variable | Funcion |
|---|---|
| `CONFIG_MODE` | `1` = modo debug/configuración, `0` = producción |
| `MQTT_DEBUG_TOPIC` | Topic donde se publica en modo debug (ej: `debug/esp32_02`) |

### Comportamiento por modo

**CONFIG_MODE=1 (debug/configuración):**
- MQTT publica a `MQTT_DEBUG_TOPIC` en lugar de `MQTT_TOPICO`
- Servidor web activo en puerto 80 (`/` y `/json`)
- Monitor serial muestra todos los datos
- El servidor NO guarda en la BD (no está suscrito a `debug/#`)

**CONFIG_MODE=0 (producción):**
- MQTT publica a `MQTT_TOPICO` (sensors/...)
- Servidor web deshabilitado (puerto 80 libre)
- El servidor guarda los datos en la BD

### Monitoreo en modo debug

```bash
mosquitto_sub -h localhost -t "debug/#"
```

### Cambio de modo

Editar `CONFIG_MODE` en `config.h` y recompilar.

## 6. Flujo de datos del nodo

Cada ciclo del `loop()`:

1. Atiende peticiones HTTP (web local) — solo si `CONFIG_MODE=1`
2. Mantiene conexion WiFi/MQTT (reconexion si es necesario)
3. Cada `CONTROL_SAMPLE_MS`: lee ADC del suelo, actualiza `lastSoilVwc`, decide si regar
4. Cada `AGGREGATION_SAMPLE_MS`: lee suelo + DHT + LDR, guarda muestra en buffer
5. Cada `MQTT_PUBLISH_INTERVAL_MS`: promedia buffer, publica JSON, resetea buffer
6. Ejecuta maquina de estados del riego

Las publicaciones MQTT incluyen: `soil_vwc` (VWC = Volumetric Water Content, nivel de agua contenida), `temperature`, `humidity` (si DHT activo),
`light_raw` y `light_percent` (si LDR activo), mas metadatos del nodo.

## 7. Checklist para agregar un nuevo ESP

En el firmware del nuevo nodo (`config.h`):
- `MQTT_CLIENT_ID = "esp32_02"`
- `MQTT_TOPICO = "sensors/esp32_02"`
- `MQTT_TOPICO_CMD = "commands/esp32_02"`
- `MQTT_STATUS_TOPIC = "devices/esp32_02/status"`

En el servidor (`config.json` de `em_server/services/mqtt_service.py`):
- Registrar los topics del nuevo nodo
- Agregar `field_mappings` si es necesario

## 8. Diagnostico rapido

```bash
# Ver telemetria en tiempo real
mosquitto_sub -t 'sensors/#' -v

# Ver datos en modo debug (CONFIG_MODE=1)
mosquitto_sub -t 'debug/#' -v

# Ver presencia de nodos
mosquitto_sub -t 'devices/+/status' -v

# Ver datos en la base de datos
sqlite3 em_server.db "SELECT source, field, COUNT(*) FROM readings GROUP BY source, field;"
```

## 9. Configuracion TLS/mTLS

Esta seccion explica como el ESP32 se conecta de forma segura al broker Mosquitto
usando mTLS (autenticacion mutua con certificados).

### 9.1 Requisitos previos

- `MQTT_PORT` debe ser `8883` (TLS) en `config.h`, NO `1883` (texto plano)
- Los certificados PEM deben existir en `firmware/esp32/certs/`
- El archivo `firmware/esp32/certs.h` debe estar generado con los PEM embebidos

### 9.2 Variables de TLS

| Variable | En `config.h` | Funcion |
|---|---|---|
| `MQTT_PORT` | `8883` | Puerto TLS. Si es 1883, TLS se deshabilita automaticamente |

El resto de la configuracion TLS esta en `certs.h` (constantes C, no editables
desde `config.h`).

### 9.3 Que es `certs.h` y como crearlo

El ESP32 **no tiene sistema de archivos** como una PC. No puede leer certificados
de archivos `.crt` o `.key`. En su lugar, los certificados se **embeben directamente
en el codigo fuente** como strings C.

El archivo `certs.h` contiene 3 constantes:

| Constante | Contenido | Usada por |
|---|---|---|
| `MQTT_CA_CERT` | CA raiz (`ca.crt`) | `wifiClient.setCACert()` — valida el certificado del broker |
| `MQTT_CLIENT_CERT` | Certificado del ESP32 (`EM-esp32_01.crt`) | `wifiClient.setCertificate()` — identidad del ESP32 |
| `MQTT_CLIENT_KEY` | Clave privada del ESP32 (`EM-esp32_01.key`) | `wifiClient.setPrivateKey()` — prueba de posesion |

#### Sintaxis C utilizada

```cpp
static const char MQTT_CA_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
(contenido PEM)
-----END CERTIFICATE-----
)EOF";
```

- **`PROGMEM`**: Almacena el dato en **flash** (memoria no volatil), NO en RAM.
  El ESP32 tiene solo ~320 KB de RAM; sin PROGMEM, los certificados la llenarian.
- **`R"EOF(...)EOF"`**: Raw string literal. Permite strings multilínea sin necesidad
  de escapar caracteres especiales (`\n`, `"`, etc.). `EOF` es un delimitador
  arbitrario; puedes usar cualquier palabra.

### 9.4 Generar `certs.h` automaticamente

Desde tu Mac (en la carpeta del proyecto), ejecuta:

```bash
python3 -c "
import sys
import os

def pem_to_c_var(filepath, var_name):
    with open(filepath, 'r') as f:
        pem = f.read().strip()
    return f'static const char {var_name}[] PROGMEM = R\"EOF(\n{pem}\n)EOF\";'

ca = pem_to_c_var('firmware/esp32/certs/ca.crt', 'MQTT_CA_CERT')
cert = pem_to_c_var('firmware/esp32/certs/EM-esp32_01.crt', 'MQTT_CLIENT_CERT')
key = pem_to_c_var('firmware/esp32/certs/EM-esp32_01.key', 'MQTT_CLIENT_KEY')

header = '''#ifndef CERTS_H
#define CERTS_H

// Certificados PEM para conexion MQTT con TLS/mTLS
// Generado automaticamente — NO editar manualmente
// Para regenerar: python3 generate_certs.py

{ca}

{cert}

{key}

#endif // CERTS_H
'''.format(ca=ca, cert=cert, key=key)

with open('firmware/esp32/certs.h', 'w') as f:
    f.write(header)
print('certs.h generado correctamente en firmware/esp32/certs.h')
"
```

### 9.5 Crear `certs.h` manualmente

Si prefieres hacerlo a mano:

1. Abre `firmware/esp32/certs/ca.crt` en un editor de texto
2. Copia **todo** el contenido (incluyendo `-----BEGIN...` y `-----END...`)
3. Pega en `certs.h` dentro de `R"EOF(...)EOF"`
4. Repite para `EM-esp32_01.crt` y `EM-esp32_01.key`

Estructura final de `certs.h`:

```cpp
#ifndef CERTS_H
#define CERTS_H

static const char MQTT_CA_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFCzCCAvOgAwIBAgIULP...
-----END CERTIFICATE-----
)EOF";

static const char MQTT_CLIENT_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIE+zCCAuOgAwIBAgIUDM...
-----END CERTIFICATE-----
)EOF";

static const char MQTT_CLIENT_KEY[] PROGMEM = R"EOF(
-----BEGIN PRIVATE KEY-----
MIIJQwIBADANBgkqhkiG9w...
-----END PRIVATE KEY-----
)EOF";

#endif // CERTS_H
```

> **IMPORTANTE:** `certs.h` esta en `.gitignore`. Nunca lo subas al repositorio
> porque contiene claves privadas.

### 9.6 Cambios en `esp32.ino` para TLS

El firmware ya incluye estos cambios. Solo para referencia, los 3 puntos clave:

**1. Includes necesarios:**

```cpp
#include <WiFi.h>
#include <WiFiClientSecure.h>   // ← soporte TLS
#include <time.h>               // ← NTP para fechas de certificados
#include <PubSubClient.h>
#include "config.h"
#include "certs.h"              // ← certificados embebidos
```

**2. Tipo de cliente WiFi:**

```diff
- WiFiClient   wifiClient;
+ WiFiClientSecure wifiClient;
```

**3. En `setup()`, despues de conectar WiFi y antes de MQTT:**

```cpp
// NTP: obligatorio para validar fechas de certificados
configTime(0, 0, "pool.ntp.org", "time.nist.gov");
delay(2000);

// Cargar certificados para mTLS
wifiClient.setCACert(MQTT_CA_CERT);
wifiClient.setCertificate(MQTT_CLIENT_CERT);
wifiClient.setPrivateKey(MQTT_CLIENT_KEY);
```

### 9.7 Flujo de conexion TLS

```
WiFi conectado
      │
      ▼
configTime()           ← sincroniza reloj via NTP (obligatorio para TLS)
      │
      ▼
setCACert()            ← carga CA para validar al broker
setCertificate()       ← carga certificado de identidad del ESP32
setPrivateKey()        ← carga clave privada para prueba de posesion
      │
      ▼
mqtt.connect()         ← handshake TLS/mTLS automatico
      │
      ▼
CONNACK (0)            ← conexion aceptada, MQTT cifrado activo
```

### 9.8 Diagnostico de TLS

Si la conexion TLS falla, revisa el **Monitor Serial** (115200 baud):

| Mensaje | Causa probable |
|---|---|
| `[NTP] Sincronizando reloj para TLS...` | NTP funcionando |
| `[TLS] Certificados cargados` | Certificados OK |
| `[MQTT] Conectando... FALLO (rc=-4)` | Error TLS: certificado invalido o expirado |
| `[MQTT] Conectando... FALLO (rc=-5)` | Error TLS:连接 rechazada (broker no acepta cert) |
| `[MQTT] Conectando... FALLO (rc=-2)` | Error de red: broker no accesible |

### 9.9 Agregar un nuevo ESP32 con TLS

Para un segundo ESP32 (ej: `esp32_02`):

1. Generar certificado: `openssl genrsa -out EM-esp32_02.key 2048`
2. Crear CSR: `openssl req -new -key EM-esp32_02.key -out EM-esp32_02.csr -subj "/CN=EM-esp32_02"`
3. Firmar: `openssl x509 -req -in EM-esp32_02.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out EM-esp32_02.crt -days 825 -sha256`
4. Copiar PEM a `firmware/esp32/certs/`
5. Regenerar `certs.h` con el script de la seccion 9.4
6. Actualizar ACL en `deploy/mosquitto/acl`:

```conf
user esp32_02
topic readwrite sensors/esp32_02
topic readwrite commands/esp32_02
topic readwrite devices/esp32_02/status
```
