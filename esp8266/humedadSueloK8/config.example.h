/*
  config.example.h
  ──────────────────────────────────────────────────────────────────
  Plantilla de configuración para humedadSueloK8.ino.

  TECNOLOGÍA: Las constantes definidas con #define son procesadas por
  el preprocesador de C++ ANTES de la compilación. No ocupan RAM en el
  microcontrolador; el compilador sustituye cada nombre por su valor
  literalmente en el código fuente. Esto es ideal para configuración
  de hardware que no cambia en tiempo de ejecución.

  USO:
    1. Copia este archivo como  humedadSueloK8/config.h
    2. Rellena los valores de tu red Wi-Fi y broker MQTT.
    3. Ajusta los valores de calibración del sensor.
    4. NO subas config.h al repositorio (ya está en .gitignore).
  ──────────────────────────────────────────────────────────────────
*/

// #pragma once — directiva que le indica al compilador que incluya
// este archivo solo una vez aunque se referencie varias veces.
// Equivale a los guardias de inclusión (#ifndef/#define/#endif).
#pragma once

// ── Wi-Fi ─────────────────────────────────────────────────────────
// El ESP8266 integra un módulo Wi-Fi 802.11 b/g/n de 2.4 GHz.
// La librería ESP8266WiFi abstrae la conexión: basta con SSID y
// contraseña para unirse a una red WPA2 doméstica.
#define WIFI_SSID      "TU_SSID_AQUI"
#define WIFI_PASSWORD  "TU_CONTRASEÑA_AQUI"

// ── MQTT ──────────────────────────────────────────────────────────
// MQTT (Message Queuing Telemetry Transport) es un protocolo de
// mensajería ligero basado en el patrón publicar/suscribir (pub/sub).
// Funciona sobre TCP/IP con un servidor central llamado "broker"
// (en este proyecto: Mosquitto corriendo en la Raspberry Pi).
//
// Ventajas de MQTT para IoT:
//   • Muy bajo consumo de ancho de banda (cabecera mínima de 2 bytes)
//   • Tolerante a redes inestables (reconexión automática)
//   • Desacopla productores y consumidores: el ESP8266 publica datos
//     sin saber quién los lee; la Raspberry Pi los recibe sin saber
//     desde cuántos dispositivos provienen.
//
// Conceptos clave:
//   BROKER  : servidor central (Mosquitto en la Raspberry Pi)
//   TÓPICO  : cadena jerárquica que identifica un canal de mensajes
//             (p. ej. "sensors/esp8266", "commands/esp8266")
//   QoS     : nivel de calidad de servicio (0=fire-and-forget, 1=al-menos-una-vez)
//   CLIENTE : cualquier dispositivo que se conecta al broker
//
// Deja MQTT_SERVER vacío ("") para deshabilitar MQTT completamente.
#define MQTT_SERVER    "192.168.1.100"   // IP de la Raspberry Pi (broker)
#define MQTT_PORT      1883              // puerto TCP estándar de MQTT sin TLS
#define MQTT_USER      ""               // vacío = sin autenticación
#define MQTT_PASSWORD  ""
// El client_id identifica de forma única a este dispositivo ante el broker.
// Si dos clientes usan el mismo ID, el broker desconectará al anterior.
#define MQTT_CLIENT_ID "esp8266-invernadero"

// Tópico de PUBLICACIÓN: el ESP8266 envía lecturas de sensor aquí.
// El servidor EM_server escucha "sensors/#" y almacena los datos.
#define MQTT_TOPICO     "sensors/esp8266"

// Tópico de SUSCRIPCIÓN: el ESP8266 escucha comandos aquí.
// La Raspberry Pi (EM_server) publica {"action":"water"} para activar el riego.
// Esto permite el control REMOTO del riego desde el dashboard web.
#define MQTT_TOPICO_CMD "commands/esp8266"

// ── Pines de hardware ─────────────────────────────────────────────
// El ESP8266 NodeMCU V3 etiqueta los pines con nombres "D0"–"D8"
// que corresponden a números GPIO internos. Usamos los números GPIO
// en el código porque Arduino los interpreta directamente.
//
//   A0       → salida analógica del sensor K8/C11
//              ¡ÚNICO pin ADC del ESP8266! Rango: 0-1023 (10 bits)
//              Corresponde al ADC interno con divisor de tensión 0-3.3 V
//   D5/GPIO14 → salida digital (DO) del sensor K8/C11
//              (umbral configurable con el potenciómetro del módulo)
//   D6/GPIO12 → señal de control del módulo relé
#define PIN_SENSOR_DO   14   // D5 en la serigrafía del NodeMCU V3
#define PIN_RELAY       12   // D6 en la serigrafía del NodeMCU V3

// ── Calibración del sensor analógico ─────────────────────────────
// El sensor K8/C11 es de tipo resistivo: su resistencia cambia según
// la humedad del suelo. El ESP8266 lee esa resistencia a través del
// ADC (Convertidor Analógico-Digital).
//
// Relación ADC ↔ humedad (INVERTIDA porque la resistencia cae al mojarse):
//   Suelo SECO    → resistencia alta → tensión alta → ADC ≈ 1023
//   Suelo MOJADO  → resistencia baja → tensión baja → ADC ≈ 300
//
// Para calibrar:
//   1. Inserta el sensor en suelo completamente seco → anota el ADC → ADC_SECO
//   2. Introduce el sensor en agua hasta el nivel máximo → anota el ADC → ADC_MOJADO
#define ADC_SECO    1023   // ← ajusta con tu medición real
#define ADC_MOJADO   300   // ← ajusta con tu medición real

// ── Umbrales de humedad (%) ───────────────────────────────────────
// Con estos umbrales el firmware decide si el suelo necesita riego:
//   humedad < UMBRAL_RIEGO → suelo demasiado seco → INICIAR riego automático
//   humedad ≥ UMBRAL_CORTE → suelo suficientemente húmedo → DETENER riego
//
// La brecha entre ambos valores (histéresis) evita que el relé
// cicle encendiéndose y apagándose rápidamente cuando la humedad
// está justo en el umbral.
#define UMBRAL_RIEGO   30    // % — por debajo → iniciar riego
#define UMBRAL_CORTE   60    // % — por encima → detener riego

// ── Tiempos (milisegundos) ────────────────────────────────────────
// Todos los tiempos son en milisegundos para poder usar millis()
// (contador de tiempo de Arduino) sin conversiones.
// El sufijo UL (Unsigned Long) es necesario porque millis() devuelve
// un unsigned long y la comparación debe ser del mismo tipo para
// evitar desbordamiento a los ~49 días de funcionamiento continuo.
#define DURACION_RIEGO_MS    10000UL  // 10 s  — tiempo máximo de riego por ciclo
#define COOLDOWN_MS         300000UL  // 5 min — espera mínima entre riegos consecutivos
#define BACKGROUND_SAMPLE_MS  30000UL  // 30 s  — intervalo de muestreo + publicación MQTT
