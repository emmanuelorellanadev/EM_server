/*
  config.example.h
  ──────────────────────────────────────────────────────────────────
  Plantilla de configuración para humedadSueloK8.ino.

  USO:
    1. Copia este archivo como  humedadSueloK8/config.h
    2. Rellena los valores de tu red Wi-Fi y broker MQTT.
    3. Ajusta los valores de calibración del sensor.
    4. NO subas config.h al repositorio (ya está en .gitignore).
  ──────────────────────────────────────────────────────────────────
*/

#pragma once

// ── Wi-Fi ─────────────────────────────────────────────────────────
#define WIFI_SSID      "TU_SSID_AQUI"
#define WIFI_PASSWORD  "TU_CONTRASEÑA_AQUI"

// ── MQTT ──────────────────────────────────────────────────────────
// Deja MQTT_SERVER vacío ("") para deshabilitar MQTT completamente.
#define MQTT_SERVER    "192.168.1.100"   // IP de la Raspberry Pi
#define MQTT_PORT      1883
#define MQTT_USER      ""                // vacío = sin autenticación
#define MQTT_PASSWORD  ""
#define MQTT_CLIENT_ID "esp8266-invernadero"
#define MQTT_TOPICO    "sensors/esp8266" // tópico de publicación

// ── Pines de hardware ─────────────────────────────────────────────
//   A0  → salida analógica del sensor K8/C11 (único pin ADC del ESP8266)
//   D5  → salida digital (DO) del sensor K8/C11  (GPIO14)
//   D6  → señal de control del relé              (GPIO12)
#define PIN_SENSOR_DO   14   // D5 en NodeMCU V3
#define PIN_RELAY       12   // D6 en NodeMCU V3

// ── Calibración del sensor analógico ─────────────────────────────
// ADC_SECO   : lectura ADC cuando el suelo está completamente seco  (≈ 1023)
// ADC_MOJADO : lectura ADC cuando el suelo está completamente húmedo (≈  300)
// Ajusta estos valores midiendo tu sensor en condiciones reales.
#define ADC_SECO    1023
#define ADC_MOJADO   300

// ── Umbrales de humedad (%) ───────────────────────────────────────
// Por debajo de UMBRAL_RIEGO se activa el riego automático.
// Por encima de UMBRAL_CORTE  se detiene el riego.
#define UMBRAL_RIEGO   30    // % — suelo seco → iniciar riego
#define UMBRAL_CORTE   60    // % — suelo húmedo → detener riego

// ── Tiempos (milisegundos) ────────────────────────────────────────
#define DURACION_RIEGO_MS    10000UL  // 10 s  — tiempo máximo de riego por ciclo
#define COOLDOWN_MS         300000UL  // 5 min — espera mínima entre riegos
#define BACKGROUND_SAMPLE_MS  30000UL  // 30 s  — intervalo de muestreo y publicación MQTT
