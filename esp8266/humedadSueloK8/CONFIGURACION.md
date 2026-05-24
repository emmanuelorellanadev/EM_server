# Configuración — humedadSueloK8

Guía completa de todas las variables que puedes ajustar en `config.h`.  
Copia `config.example.h` → `config.h` y edita los valores según tu instalación.

---

## 1. Wi-Fi

| Variable    | Tipo   | Descripción                          |
|-------------|--------|--------------------------------------|
| `WIFI_SSID` | string | Nombre de tu red Wi-Fi (SSID)        |
| `WIFI_PASS` | string | Contraseña de la red Wi-Fi           |

```c
#define WIFI_SSID "MiRed"
#define WIFI_PASS "mi_contrasena"
```

---

## 2. Pines GPIO

| Variable    | Default | Descripción                                              |
|-------------|---------|----------------------------------------------------------|
| `PIN_DO`    | 14      | Salida digital del sensor K8/C11 (D5 en NodeMCU V3)     |
| `PIN_RELAY` | 5       | Control del relé (D1 en NodeMCU V3), active-high        |
| `PIN_LED`   | `LED_BUILTIN` | LED interno del NodeMCU (active-low)              |

> **Lógica del relé (active-high):**  
> `HIGH` → relé ACTIVO → válvula ABIERTA  
> `LOW`  → relé INACTIVO → válvula CERRADA *(estado seguro al arranque)*

---

## 3. Calibración ADC

El ADC del ESP8266 devuelve valores entre 0 y 1023 según la tensión en A0.

| Variable  | Default | Descripción                                      |
|-----------|---------|--------------------------------------------------|
| `RAW_DRY` | 571     | Lectura ADC con el sensor en aire (seco)         |
| `RAW_WET` | 336     | Lectura ADC con el sensor sumergido en agua      |

**Procedimiento de calibración:**
1. Conecta el ESP8266 y abre el Monitor Serie (115200 baud).
2. Deja el sensor en el aire durante 30 s → anota el valor `Raw`.  
   Escríbelo en `RAW_DRY`.
3. Sumerge el sensor en un vaso de agua → anota el valor `Raw`.  
   Escríbelo en `RAW_WET`.

⚠ Usar valores incorrectos genera lecturas erróneas y riegos indeseados.

---

## 4. Umbrales de humedad

| Variable               | Default | Descripción                                                    |
|------------------------|---------|----------------------------------------------------------------|
| `ON_THRESHOLD_PERCENT` | 35      | Porcentaje mínimo: si baja de aquí se activa el riego          |
| `OFF_THRESHOLD_PERCENT`| 45      | Porcentaje informativo para mostrar "HUMEDO" en la interfaz    |

> `OFF_THRESHOLD_PERCENT` **no controla el relé**; el riego siempre termina
> por tiempo (`RELAY_ON_TIME_MS`).

---

## 5. Tiempos del riego

| Variable           | Default  | Descripción                                           |
|--------------------|----------|-------------------------------------------------------|
| `RELAY_ON_TIME_MS` | 5000 ms  | Tiempo que permanece abierta la válvula por ciclo     |
| `COOLDOWN_MS`      | 15000 ms | Espera mínima entre dos riegos consecutivos           |

Ajusta `RELAY_ON_TIME_MS` según el caudal de tu válvula y el tipo de suelo.

---

## 6. Lectura ADC — estabilidad de señal

| Variable         | Default | Descripción                                            |
|------------------|---------|--------------------------------------------------------|
| `ANALOG_SAMPLES` | 20      | Número de muestras promediadas por lectura             |
| `ANALOG_DELAY_MS`| 5       | Retardo entre muestras consecutivas (ms)               |

El promedio reduce el ruido inherente del ADC de 10 bits del ESP8266.

---

## 7. Muestreo en segundo plano

| Variable               | Default | Descripción                                             |
|------------------------|---------|---------------------------------------------------------|
| `BACKGROUND_SAMPLE_MS` | 3000 ms | Intervalo entre lecturas del sensor (y publicación MQTT)|

---

## 8. MQTT

> **Librería requerida:** instala **PubSubClient** de *Nick O'Leary* desde
> el Library Manager de Arduino IDE antes de compilar.

El firmware publica un JSON al broker cada `BACKGROUND_SAMPLE_MS`.

**Formato del mensaje:**
```json
{
  "percent": 51.2,
  "state": "WET",
  "watering": false,
  "last_watered_sec": 120,
  "on_threshold_percent": 35,
  "relay_on_time_s": 1.0
}
```

Valores posibles de `state`: `WET`, `DRY`, `WATERING`, `COOLDOWN`.

| Variable          | Default                  | Descripción                                          |
|-------------------|--------------------------|------------------------------------------------------|
| `MQTT_SERVER`     | `"192.168.1.100"`        | IP o hostname del broker (vacío `""` → MQTT inactivo)|
| `MQTT_PORT`       | 1883                     | Puerto TCP del broker                                |
| `MQTT_CLIENT_ID`  | `"esp8266_suelo"`        | ID único del cliente (único por dispositivo)         |
| `MQTT_TOPICO`     | `"sensors/esp8266"`      | Tópico donde se publican los datos                   |
| `MQTT_TOPICO_CMD` | `"commands/esp8266"`     | Tópico donde escucha comandos remotos                |
| `MQTT_STATUS_TOPIC` | `"devices/esp8266/status"` | Tópico de presencia (`online`/`offline`)          |
| `MQTT_USER`       | `""`                     | Usuario del broker (vacío si no requiere auth)       |
| `MQTT_PASS_BROKER`| `""`                     | Contraseña del broker (vacío si no requiere auth)    |

### Reconexión automática

Si el broker no está disponible al arrancar, el firmware reintenta la
conexión cada `MQTT_RECONNECT_INTERVAL_MS` (5 s) sin bloquear el
servidor web ni el control del riego.

Al reconectar:
- el firmware vuelve a suscribirse a `MQTT_TOPICO_CMD`,
- publica `online` (retained) en `MQTT_STATUS_TOPIC`,
- y envía de inmediato el último payload de sensores.

Si el dispositivo se reinicia bruscamente o pierde energía, el broker
publicará `offline` por LWT en `MQTT_STATUS_TOPIC`.

### Deshabilitar MQTT

Deja `MQTT_SERVER` vacío para que toda la lógica MQTT quede inactiva:

```c
#define MQTT_SERVER ""
```

---

## Resumen rápido

```
config.h
├── Wi-Fi          → WIFI_SSID / WIFI_PASS
├── Pines          → PIN_DO / PIN_RELAY / PIN_LED
├── Calibración    → RAW_DRY / RAW_WET
├── Umbrales       → ON_THRESHOLD_PERCENT / OFF_THRESHOLD_PERCENT
├── Riego          → RELAY_ON_TIME_MS / COOLDOWN_MS
├── ADC            → ANALOG_SAMPLES / ANALOG_DELAY_MS
├── Muestreo       → BACKGROUND_SAMPLE_MS
└── MQTT           → MQTT_SERVER / MQTT_PORT / MQTT_CLIENT_ID /
                     MQTT_TOPICO / MQTT_TOPICO_CMD / MQTT_STATUS_TOPIC /
                     MQTT_USER / MQTT_PASS_BROKER
```
