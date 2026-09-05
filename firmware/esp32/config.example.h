#pragma once
/*
  config.example.h — Plantilla de configuración para EM_server ESP32
  ──────────────────────────────────────────────────────────────────
  INSTRUCCIONES:
    1. Copia este archivo: cp config.example.h config.h
    2. Edita config.h con tus valores reales (SSID, contraseña, IP, etc.).
    3. NUNCA subas config.h al repositorio — ya está en .gitignore para
       proteger tus credenciales y tu red Wi-Fi.

  Este archivo sirve como PLANTILLA pública con valores de ejemplo.
  Todo está comentado didácticamente para que puedas ajustarlo sin
  necesidad de entender el código .ino principal.

  #pragma once evita que este archivo se incluya más de una vez si
  por accidente se referencia desde varios lugares (include guard).
*/

// ================================================================
// Wi-Fi  —  Credenciales de tu red local
// ================================================================
#define WIFI_SSID "TU_RED_WIFI"      // Nombre de tu red (SSID)
#define WIFI_PASS "TU_CONTRASENA"    // Contraseña de tu red

// ================================================================
// Pines  —  Conexión física ESP32 ↔ componentes
// ================================================================
// IMPORTANTE: Usamos la numeración GPIO real, NO la etiqueta impresa en la placa.

// ── Sensor de suelo K8/C11 ──────────────────────────────────────
// El sensor tiene dos salidas:
//   AO (Analog Output) → conectar a PIN_AO
//   DO (Digital Output) → conectar a PIN_DO
// DO genera HIGH/LOW según un potenciómetro de umbral en el sensor.
// En este firmware DO es solo informativo; el control real del riego
// viene de la lectura analógica (más precisa).
#define PIN_DO 36

// PIN_AO: pin analógico del sensor de suelo.
// GPIO 34 = ADC1_CH6 (D34). Debe ser ADC1 (GPIO 32-39): los pines ADC2
// devuelven 0 mientras el WiFi está activo.
// Cableado: AO del sensor K8/C11 → D34.
#define PIN_AO 34

// ── Relé ────────────────────────────────────────────────────────
// El relé es un interruptor electromecánico que permite al
// microcontrolador (3.3 V) controlar cargas de alto voltaje
// (12 V DC de la electroválvula) de forma segura.
//
// Este módulo relé es ACTIVE-HIGH:
//   HIGH → bobina energizada → contacto cierra → electroválvula recibe 12 V
//   LOW  → bobina apagada → contacto abre → electroválvula sin corriente
#define PIN_RELAY 12

// ── LED integrado ───────────────────────────────────────────────
#define PIN_LED 2
#define LED_ACTIVE_LOW 0   // 0 = active-high (ESP32 DevKit V1)

// ── Sensor ambiental DHT11/DHT22 (opcional) ─────────────────────
// Habilita lectura de temperatura y humedad ambiental.
// Requiere instalar la librería "DHT sensor library" de Adafruit.
#define ENABLE_AMBIENT_SENSOR 1
#define PIN_AMBIENT 27
#define AMBIENT_SENSOR_DHT22 1   // 1=DHT22, 0=DHT11

// ── Sensor de luz LDR (opcional) ────────────────────────────────
// LDR (fotorresistor) + resistencia de 10K formando un divisor de
// tensión. Con pulldown a GND:
//   Más luz → LDR baja resistencia → más voltaje en el pin → raw alto
//   Oscuridad → LDR alta resistencia → menos voltaje → raw bajo
#define ENABLE_LIGHT_SENSOR 1
#define PIN_LIGHT 35

// ================================================================
// Calibración ADC  ← ¡AJUSTA ESTO SEGÚN TUS SENSORES!
// ================================================================

// ── Sensor de suelo ─────────────────────────────────────────────
// El ADC lee voltajes en PIN_AO y los convierte a un valor digital.
// El sensor de humedad genera un voltaje que varía según la humedad:
//   Suelo SECO  → alta resistencia → más voltaje → ADC da valor ALTO
//   Suelo HÚMEDO → baja resistencia → menos voltaje → ADC da valor BAJO
//
// PROCEDIMIENTO DE CALIBRACIÓN (hazlo una sola vez):
//   1. Conecta el sensor a la placa y abre el Monitor Serie (115200 baud).
//   2. Pon el sensor en el AIRE (completamente seco): anota el valor Raw.
//      → Ese valor es tu RAW_DRY.
//   3. Sumerge el sensor en un vaso de AGUA: anota el valor Raw.
//      → Ese valor es tu RAW_WET.
//   4. Reemplaza los valores de ejemplo abajo con los tuyos.
//
// ⚠ Los valores aquí son de EJEMPLO. Tu sensor puede dar valores
//   diferentes. Usar valores incorrectos produce lecturas erróneas.
#define RAW_DRY 3130   // ADC con sensor seco (en aire)
#define RAW_WET 1075   // ADC con sensor húmedo (en agua)

// ── Sensor de luz LDR ──────────────────────────────────────────
// El LDR con pulldown de 10K funciona al revés del sensor de suelo:
//   Oscuridad → voltaje bajo → raw bajo (~500)
//   Luz máxima → voltaje alto → raw alto (~3000)
//
// PROCEDIMIENTO DE CALIBRACIÓN:
//   1. Cubre completamente el LDR (oscuridad total): anota el Raw.
//      → Ese valor es tu LIGHT_DARK_RAW.
//   2. Apunta una linterna directamente al LDR (luz máxima): anota el Raw.
//      → Ese valor es tu LIGHT_BRIGHT_RAW.
//   3. Reemplaza los valores de ejemplo con los tuyos.
#define LIGHT_DARK_RAW   500   // ADC a oscuras
#define LIGHT_BRIGHT_RAW 3000  // ADC con luz máxima

// ── Offsets opcionales para el DHT ──────────────────────────────
// Ejemplo: si marca +1.0 °C de más, usar TEMP_OFFSET_C -1.0
//          si marca -3 %RH de menos, usar HUM_OFFSET_PCT +3.0
#define TEMP_OFFSET_C    0.0f
#define HUM_OFFSET_PCT   0.0f

// ================================================================
// Umbrales de riego  (soil_vwc 0–100 %)
// ================================================================
// ON_THRESHOLD_SOIL_VWC: si el VWC (Volumetric Water Content) del
//   suelo baja de este valor, el sistema abre la electroválvula.
//   ⚠ Muy bajo → la planta puede secarse.
//     Muy alto → riega demasiado (puede pudrir raíces).
#define ON_THRESHOLD_SOIL_VWC  35

// OFF_THRESHOLD_SOIL_VWC: solo informativo (web + JSON).
//   No controla el relé. El riego siempre dura RELAY_ON_TIME_MS.
#define OFF_THRESHOLD_SOIL_VWC 45

// ================================================================
// Tiempos del riego
// ================================================================
// RELAY_ON_TIME_MS: tiempo que permanece abierta la electroválvula.
//   Cómo calcularlo: (agua_litros_por_ciclo / caudal_litros_por_seg) × 1000
//   Ej: 0.2 L/s, 1 L por ciclo → 1/0.2 × 1000 = 5000 ms
#define RELAY_ON_TIME_MS   5000UL

// COOLDOWN_MS: espera después de regar antes de permitir otro ciclo.
//   Evita riegos en bucle. Recomendación: 2-5× RELAY_ON_TIME_MS.
#define COOLDOWN_MS       15000UL

// ================================================================
// Estabilización de lecturas (promedio de N muestras)
// ================================================================
// Más muestras = más estable pero más lento.
#define ANALOG_SAMPLES   20    // Lecturas ADC por muestra (suelo + luz)
#define ANALOG_DELAY_MS   5    // ms entre lecturas ADC
#define AMBIENT_SAMPLES   1    // Lecturas DHT por muestra
#define AMBIENT_DELAY_MS  0  // ms entre lecturas DHT

// ================================================================
// Temporizadores de muestreo y publicación MQTT
// ================================================================
// El sistema usa tres ritmos independientes:
//
// CONTROL_SAMPLE_MS
//   → Lectura rápida solo para decisión de riego local.
//   → NO afecta la ventana MQTT.
//   → Valor recomendado: 5-30 segundos.
//
// AGGREGATION_SAMPLE_MS
//   → Cada cuánto se captura una muestra COMPLETA (suelo + DHT + LDR)
//     y se guarda en el buffer circular de la ventana MQTT.
//   → Las muestras se acumulan hasta completar la ventana.
//
// MQTT_PUBLISH_INTERVAL_MS
//   → Cada cuánto se calcula el PROMEDIO de todas las muestras de la
//     ventana y se publica por MQTT.
//
// RELACIÓN OBLIGATORIA (validada en compilación):
//   MQTT_WINDOW_SAMPLE_COUNT = MQTT_PUBLISH_INTERVAL_MS / AGGREGATION_SAMPLE_MS
//
// Ejemplo con ventana de 6 muestras, muestra cada 30 s, publica cada 3 min:
#define CONTROL_SAMPLE_MS        10000UL
#define AGGREGATION_SAMPLE_MS   30000UL    // cada 30 s se captura una muestra
#define MQTT_PUBLISH_INTERVAL_MS 180000UL  // cada 3 min se publica el promedio
#define MQTT_WINDOW_SAMPLE_COUNT 6         // 180000 / 30000 = 6 muestras por ventana

// ================================================================
// Modo configuración
// ================================================================
// CONFIG_MODE = 1 → publica MQTT a topic de debug (no guarda en BD del servidor)
// CONFIG_MODE = 0 → producción: publica al topic normal (sensors/...)
//
// Cuando CONFIG_MODE está activo, el ESP32 sigue publicando por MQTT
// y mostrando datos en el Monitor Serie, pero el servidor NO guarda
// esos datos en la base de datos porque no está suscrito al topic
// de debug.
//
// Para monitorear datos en modo debug:
//   mosquitto_sub -h localhost -t "debug/#"
#define CONFIG_MODE 0

// Topic de debug (solo se usa cuando CONFIG_MODE = 1).
// Debe ser diferente al topic normal (sensors/...) para que el
// servidor no lo reciba ni lo guarde.
#define MQTT_DEBUG_TOPIC "debug/esp32_01"

// ================================================================
// Tiempos de reconexión
// ================================================================
#define MQTT_RECONNECT_INTERVAL_MS 5000UL
#define WIFI_RECONNECT_INTERVAL_MS 5000UL
#define WIFI_BOOT_TIMEOUT_MS      30000UL

// ================================================================
// MQTT  —  Comunicación con la Raspberry Pi vía broker
// ================================================================
// MQTT (Message Queuing Telemetry Transport) es un protocolo de
// mensajería ligero basado en el patrón publicar/suscribir (pub/sub).
// Funciona sobre TCP/IP con un servidor central llamado "broker"
// (en este proyecto: Mosquitto corriendo en la Raspberry Pi).
//
// VENTAJAS:
//   • Cabecera mínima de 2 bytes (bajo consumo de ancho de banda)
//   • Reconexión automática (tolerante a redes inestables)
//   • Desacopla productores y consumidores
//
// CÓMO FUNCIONA EN ESTE PROYECTO:
//
//   PUBLICACIÓN (ESP32 → broker):
//     Cada MQTT_PUBLISH_INTERVAL_MS se envía un JSON con el promedio
//     de la ventana. La Raspberry Pi (mqtt_client.py) escucha
//     "sensors/#" y guarda en SQLite.
//
//   SUSCRIPCIÓN (broker → ESP32):
//     El dispositivo escucha comandos en MQTT_TOPICO_CMD.
//     Comando: {"action":"water"} → activa el relé.
//
//   PRESENCIA:
//     Al conectarse publica "online" con retain.
//     Si se desconecta abruptamente, el broker publica "offline" (LWT).
//
// ¿CÓMO DESHABILITAR MQTT?
//   Deja MQTT_SERVER vacío: #define MQTT_SERVER ""
//   El firmware omitirá toda la lógica MQTT automáticamente.
//
// Instala la librería "PubSubClient" de Nick O'Leary desde el
// Library Manager de Arduino IDE antes de compilar.

// IP del broker MQTT (tu Raspberry Pi).
//   Puedes verla con: hostname -I (en la terminal de la RPi)
//   Dejar "" para deshabilitar MQTT.
#define MQTT_SERVER      "192.168.1.2"

#define MQTT_PORT        8883   // 8883 = con TLS/mTLS (requiere certs.h)
                               // 1883 = sin TLS (solo para desarrollo local)

// Identificador único de ESTE dispositivo ante el broker.
// Si dos dispositivos usan el mismo CLIENT_ID, el broker desconecta
// al anterior. Cambia este nombre si tienes varios nodos.
#define MQTT_CLIENT_ID   "esp32_01"

// Tópico de publicación: el ESP32 envía sus datos aquí.
#define MQTT_TOPICO      "sensors/esp32_01"

// Tópico de suscripción: el ESP32 escucha comandos aquí.
#define MQTT_TOPICO_CMD  "commands/esp32_01"

// Tópico de presencia (online/offline, LWT).
#define MQTT_STATUS_TOPIC "devices/esp32_01/status"

// Credenciales opcionales del broker.
#define MQTT_USER        ""    // Dejar "" si no hay autenticación
#define MQTT_PASS_BROKER ""    // Dejar "" si no hay autenticación
