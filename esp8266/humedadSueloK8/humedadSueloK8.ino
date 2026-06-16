/*
  humedadSueloK8.ino
  ──────────────────────────────────────────────────────────────────
  Monitor de humedad de suelo con servidor web, riego temporizado
  y comunicación bidireccional vía MQTT.

  ¿QUÉ HACE ESTE FIRMWARE?
  ─────────────────────────
  1. Lee continuamente un sensor de humedad de suelo (señal analógica).
  2. Controla automáticamente un relé (válvula de riego) cuando el
     suelo está demasiado seco.
  3. Sirve una página web local para monitorear el estado en tiempo real.
  4. Publica los datos del sensor a una Raspberry Pi vía MQTT (protocolo
     de mensajería ligero para IoT).
  5. Recibe comandos de riego remotos desde la Raspberry Pi vía MQTT.

  HARDWARE NECESARIO:
  ────────────────────
    • ESP8266 NodeMCU V3 o ESP32 Dev Module
        Seleccionar en Arduino IDE la placa real de tu hardware.
        Este firmware comparte la misma lógica para ambas placas y usa
        parámetros definidos en config.h (pines, MQTT, calibración, etc.).

    • Sensor capacitivo/resistivo K8 o C11
        AO (Analog Output) → PIN_AO : señal analógica según humedad
        DO (Digital Output) → PIN_DO : salida digital (umbral fijo, no usado en control)
        VCC → 3.3 V o 5 V según modelo
        GND → GND

    • Módulo relé 5 V
        IN  → PIN_RELAY : señal de control desde la placa
        VCC → 5 V externo  : alimentación de la bobina del relé
        GND → GND compartido con la placa
        Contactos NO/COM   : conectados a la electroválvula
        IMPORTANTE: Este módulo es active-HIGH (HIGH activa el relé).

    • Electroválvula 12 V DC
        Conectar en serie con una fuente de 12 V a través de los contactos
        NO (Normally Open) y COM del relé.
        ⚠ Añadir un diodo flyback en paralelo con la electroválvula para
          proteger el relé de los picos de voltaje al desactivar la bobina.

  CÓMO COMPILAR Y SUBIR:
  ───────────────────────
    1. Copia humedadSueloK8/config.example.h → humedadSueloK8/config.h
    2. Edita config.h con tu SSID, contraseña Wi-Fi, calibración del sensor
       y datos del broker MQTT (IP de la Raspberry Pi).
    3. En Arduino IDE → Herramientas → Gestor de librerías:
         Instala "PubSubClient" de Nick O'Leary (v2.8 o superior).
    4. Selecciona la placa (ESP8266 o ESP32), compila y sube.

  ENDPOINTS DEL SERVIDOR WEB LOCAL:
  ────────────────────────────────────
    GET /       → Página HTML amigable con estado actual del sistema.
                  Abrir en el navegador con la IP que muestra el Monitor Serie.
    GET /json   → Respuesta JSON con todos los datos. Útil para dashboards
                  o integrar con otros sistemas sin necesidad de MQTT.

  COMUNICACIÓN MQTT (pub/sub):
  ──────────────────────────────
    PUBLICACIÓN (Dispositivo → Raspberry Pi):
      Tópico : definido por MQTT_TOPICO en config.h
      Cada   : BACKGROUND_SAMPLE_MS milisegundos
      Formato: {"percent":65.3,"state":"WET","watering":false,
                "on_threshold_percent":35,"relay_on_time_s":1.0}

    SUSCRIPCIÓN (Raspberry Pi → Dispositivo):
      Tópico  : definido por MQTT_TOPICO_CMD en config.h
      Comando : {"action":"water"}
      Efecto  : activa el riego remotamente (igual que si el sensor detectara suelo seco)

    Si MQTT_SERVER está vacío ("") en config.h, se omite toda la lógica
    MQTT y el firmware funciona solo con servidor web y control local.
  ──────────────────────────────────────────────────────────────────
*/

// ================================================================
// LIBRERÍAS
// ================================================================
// #include carga código externo necesario para el firmware.
// Las librerías de sistema van entre < > y las propias entre " ".

#if defined(ESP8266)
#include <ESP8266WiFi.h>      // Maneja la conexión Wi-Fi del ESP8266.
#include <ESP8266WebServer.h> // Servidor HTTP para ESP8266.
using LocalWebServer = ESP8266WebServer;
#elif defined(ESP32)
#include <WiFi.h>             // Maneja la conexión Wi-Fi del ESP32.
#include <WebServer.h>        // Servidor HTTP para ESP32.
using LocalWebServer = WebServer;
#else
#error "Este firmware soporta solo ESP8266 o ESP32"
#endif
                               // Permite conectarse a una red, obtener IP, etc.
                               // Con él el microcontrolador puede responder a
                               // peticiones GET desde un navegador web.
#include <PubSubClient.h>      // Librería MQTT de Nick O'Leary.
                               // Permite publicar mensajes y suscribirse a tópicos
                               // en un broker MQTT (p. ej. Mosquitto en la RPi).
#include "config.h"            // Archivo de configuración PERSONAL (no subir a git).
                               // Contiene: SSID, contraseña Wi-Fi, pines, umbrales,
                               // tiempos y parámetros MQTT. Créalo copiando
                               // config.example.h y editando con tus valores.
#include <math.h>

#if ENABLE_AMBIENT_SENSOR
#include <DHT.h>
#endif

// ================================================================
// OBJETOS GLOBALES
// ================================================================

// ── Servidor web ─────────────────────────────────────────────────
// LocalWebServer gestiona peticiones HTTP entrantes.
// El parámetro 80 es el puerto TCP estándar para HTTP.
// Los clientes (navegadores) se conectan a http://<IP_del_ESP>/
LocalWebServer server(80);

// ── Clientes MQTT ────────────────────────────────────────────────
// WiFiClient es la capa de transporte TCP/IP sobre Wi-Fi.
// PubSubClient usa ese canal TCP para hablar el protocolo MQTT.
// Separamos los dos objetos para poder reutilizar WiFiClient si
// necesitáramos otras conexiones TCP en el futuro.
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);  // mqtt es el objeto que publica/suscribe

// Client ID MQTT derivado dinámicamente para evitar colisiones entre
// dispositivos que compartan el mismo config.h.
char mqttClientIdDynamic[32] = {0};

// Temporizador de reconexión MQTT.
// millis() devuelve los ms transcurridos desde el arranque (tipo unsigned long).
// Guardamos el instante del último intento de reconexión para no bloquear
// el loop() mientras esperamos entre reintentos.
unsigned long lastMqttAttemptMs = 0;

// Temporizador de reconexión Wi-Fi (runtime).
unsigned long lastWifiAttemptMs = 0;

// ================================================================
// MÁQUINA DE ESTADOS DEL RIEGO
// ================================================================
// Una "máquina de estados" es una forma de organizar el comportamiento
// del sistema. En lugar de usar muchas banderas booleanas (isWatering,
// isCooldown…), usamos un único valor que indica en qué fase se está.
//
// Diagrama de estados:
//
//   ┌──────┐  pct < UMBRAL  ┌──────────┐  tiempo >= RELAY_ON  ┌──────────┐
//   │ IDLE │──────────────→ │ WATERING │────────────────────→  │ COOLDOWN │
//   │      │  o cmd MQTT    │          │                        │          │
//   └──────┘                └──────────┘                        └──────────┘
//      ↑                                                              │
//      └──────────────────────── tiempo >= COOLDOWN_MS ──────────────┘
//
// IDLE     : El sistema está en reposo, monitoreando la humedad.
//            Si el porcentaje cae bajo el umbral (suelo seco) → WATERING.
//            También puede llegar aquí por comando MQTT {"action":"water"}.
// WATERING : El relé está activo y la válvula abierta.
//            Cuando transcurre RELAY_ON_TIME_MS → cierra la válvula → COOLDOWN.
// COOLDOWN : Período de espera para que el suelo absorba el agua.
//            Evita que el sistema active el riego en bucle continuo.
//            Cuando transcurre COOLDOWN_MS → vuelve a IDLE.

enum RelayState {
  IDLE,       // Reposo: esperando que la humedad baje del umbral
  WATERING,   // Regando: válvula abierta, relé activo, LED encendido
  COOLDOWN    // Enfriamiento: válvula cerrada, esperando antes del siguiente ciclo
};

// Estado actual del sistema. Arranca en IDLE (seguro, válvula cerrada).
RelayState relayState = IDLE;

// Marcas de tiempo para controlar las duraciones de cada fase.
// millis() en Arduino es como un reloj desde el arranque (no bloqueante).
// Usamos unsigned long para aguantar ~49 días sin desbordamiento.
unsigned long relayStartMs    = 0;  // cuándo comenzó el riego actual
unsigned long cooldownStartMs = 0;  // cuándo comenzó el cooldown actual
unsigned long lastWaterEndMs  = 0;  // cuándo terminó el último riego (0 = nunca)

// ================================================================
// VARIABLES DE LECTURA DEL SENSOR
// ================================================================
// Guardamos la última lectura en variables globales para poder
// acceder a ellas desde el servidor web, MQTT y el control del relé
// sin necesidad de releer el sensor en cada función (más eficiente).

float         lastPercent  = 0.0f;  // Última humedad en % (0.0 = seco, 100.0 = saturado)
int           lastRaw      = 0;     // Último valor crudo del ADC (0–1023)
unsigned long lastSampleMs = 0;     // Momento de la última lectura (millis)

float         lastAmbientTempC    = NAN;
float         lastAmbientHumPct   = NAN;
unsigned long lastAmbientSampleMs = 0;

#if ENABLE_AMBIENT_SENSOR
#if AMBIENT_SENSOR_DHT22
#define AMBIENT_SENSOR_TYPE DHT22
#else
#define AMBIENT_SENSOR_TYPE DHT11
#endif
DHT ambientSensor(PIN_AMBIENT, AMBIENT_SENSOR_TYPE);

bool readAmbient(float& outTempC, float& outHumPct) {
  // DHT puede fallar por timing; tomamos varias muestras y promediamos.
  float sumTemp = 0.0f;
  float sumHum = 0.0f;
  int validSamples = 0;

  for (int i = 0; i < AMBIENT_SAMPLES; i++) {
    float t = ambientSensor.readTemperature();
    float h = ambientSensor.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      sumTemp += t;
      sumHum += h;
      validSamples++;
    }
    if (i < AMBIENT_SAMPLES - 1) {
      delay(AMBIENT_DELAY_MS);
    }
  }

  if (validSamples == 0) return false;

  outTempC = (sumTemp / (float)validSamples) + TEMP_OFFSET_C;
  outHumPct = (sumHum / (float)validSamples) + HUM_OFFSET_PCT;

  if (outHumPct < 0.0f) outHumPct = 0.0f;
  if (outHumPct > 100.0f) outHumPct = 100.0f;

  return true;
}
#endif

uint32_t getDeviceIdSuffix() {
#if defined(ESP8266)
  return ESP.getChipId();
#else
  return (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFUL);
#endif
}

// ================================================================
// readADC()  —  Lectura promediada del sensor de humedad
// ================================================================
// ¿POR QUÉ PROMEDIAR?
//   El ADC (Convertidor Analógico-Digital) puede tener ruido
//   eléctrico. Una sola lectura puede variar ±10 unidades. Tomar
//   ANALOG_SAMPLES lecturas y promediarlas da un valor más estable.
//
// ¿POR QUÉ delay() ENTRE MUESTRAS?
//   Después de una lectura el condensador interno del ADC necesita
//   un pequeño tiempo para "recargarse". ANALOG_DELAY_MS (5 ms por
//   defecto) evita leer el mismo valor repetido.
//
// RETORNO: entero leído del ADC según resolución configurada por placa.
//   • Suelo SECO  → valor ALTO  (poca conductividad → más voltaje → ~571)
//   • Suelo HÚMEDO → valor BAJO (más conductividad → menos voltaje → ~336)
//   (Esto parece contraintuitivo, pero es la lógica del sensor resistivo)
// ================================================================
int readADC() {
  long sum = 0;
  for (int i = 0; i < ANALOG_SAMPLES; i++) {
    sum += analogRead(PIN_AO);    // Lee el pin analógico configurado
    delay(ANALOG_DELAY_MS);   // Pequeña pausa para estabilidad del ADC
  }
  return (int)(sum / ANALOG_SAMPLES);  // Devuelve el promedio entero
}

void setStatusLed(bool on) {
  if (PIN_LED < 0) return;
  if (PIN_LED == PIN_RELAY) return;
#if LED_ACTIVE_LOW
  digitalWrite(PIN_LED, on ? LOW : HIGH);
#else
  digitalWrite(PIN_LED, on ? HIGH : LOW);
#endif
}

// ================================================================
// rawToPercent(raw)  —  Conversión ADC → porcentaje de humedad
// ================================================================
// Aplica una interpolación lineal entre los dos puntos de calibración:
//
//   raw = RAW_DRY  →  0 %   (sensor en aire, completamente seco)
//   raw = RAW_WET  →  100 % (sensor sumergido en agua)
//
// FÓRMULA:
//   pct = (RAW_DRY - raw) / (RAW_DRY - RAW_WET) × 100
//
//   Ejemplo con RAW_DRY=571, RAW_WET=336 y una lectura raw=450:
//   pct = (571 - 450) / (571 - 336) × 100
//       = 121 / 235 × 100
//       ≈ 51.5 %
//
// RECORTE AL RANGO [0, 100]:
//   Si el sensor da un valor fuera del rango calibrado (por ruido o
//   posición incorrecta), la fórmula puede dar negativos o >100.
//   El recorte (clamp) evita mostrar valores absurdos.
//
// PARÁMETRO: raw  — valor crudo del ADC (0–1023)
// RETORNO  : float en [0.0, 100.0] representando % de humedad
// ================================================================
float rawToPercent(int raw) {
  // Interpolación lineal inversa (valores altos = seco = 0%)
  float pct = (float)(RAW_DRY - raw) / (float)(RAW_DRY - RAW_WET) * 100.0f;

  // Recortar al rango válido para evitar valores absurdos por ruido o mala calibración
  if (pct < 0.0f)   pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;

  return pct;
}

// ================================================================
// updateRelay(pct)  —  Control del relé y el LED según la humedad
// ================================================================
// Esta función implementa la máquina de estados descrita arriba.
// Se llama en cada iteración del loop() con el porcentaje actual.
//
// LÓGICA DEL PIN DE RELÉ (active-HIGH en este módulo):
//   digitalWrite(PIN_RELAY, HIGH) → bobina del relé energizada
//                                  → contacto NO cierra el circuito
//                                  → electroválvula recibe corriente
//                                  → VÁLVULA ABIERTA (agua fluye)
//
//   digitalWrite(PIN_RELAY, LOW)  → bobina desactivada
//                                  → contacto NO abre el circuito
//                                  → electroválvula sin corriente
//                                  → VÁLVULA CERRADA (estado seguro)
//
// LED INTEGRADO:
//   La polaridad se define en config.h con LED_ACTIVE_LOW para soportar
//   NodeMCU (active-low) y ESP32 Dev Module (normalmente active-high).
//
// USO DE millis() EN LUGAR DE delay():
//   delay(5000) bloquea el procesador 5 s — durante ese tiempo no
//   se atienden peticiones web ni mensajes MQTT.
//   En su lugar guardamos el instante de inicio (relayStartMs) y en
//   cada llamada calculamos si ya pasó el tiempo suficiente.
//   Esto se llama "multitarea cooperativa no bloqueante".
//
// PARÁMETRO: pct — humedad actual en % (0.0 = seco, 100.0 = húmedo)
// ================================================================
void updateRelay(float pct) {
  unsigned long now = millis();  // Tiempo actual en ms desde el arranque

  switch (relayState) {

    // ── IDLE: sistema en reposo, monitoreando ───────────────────
    case IDLE:
      if (pct < ON_THRESHOLD_PERCENT) {
        // El suelo está más seco que el umbral → iniciar riego
        digitalWrite(PIN_RELAY, HIGH);  // HIGH → relé activo → válvula abierta
        relayStartMs = now;             // Registrar cuándo comenzó el riego
        relayState   = WATERING;        // Transición al estado WATERING
        Serial.printf("[RIEGO] Iniciado. Humedad: %.1f%%\n", pct);
      }
      break;

    // ── WATERING: válvula abierta, contando tiempo ──────────────
    case WATERING:
      // now - relayStartMs: ms transcurridos desde que se abrió la válvula
      if (now - relayStartMs >= RELAY_ON_TIME_MS) {
        // Ya regó suficiente → cerrar la válvula
        digitalWrite(PIN_RELAY, LOW);   // LOW → relé inactivo → válvula cerrada
        lastWaterEndMs  = now;          // Guardar cuándo terminó este riego
        cooldownStartMs = now;          // Iniciar conteo del cooldown
        relayState      = COOLDOWN;     // Transición al estado COOLDOWN
        Serial.println("[RIEGO] Terminado. Iniciando cooldown.");
      }
      break;

    // ── COOLDOWN: válvula cerrada, esperando antes del siguiente ciclo ──
    case COOLDOWN:
      if (now - cooldownStartMs >= COOLDOWN_MS) {
        // El tiempo de espera terminó → volver a monitorear
        relayState = IDLE;
        Serial.println("[RIEGO] Cooldown finalizado. Listo para siguiente ciclo.");
      }
      break;
  }

  // LED: refleja visualmente si el riego está activo.
  // Operador ternario: condición ? valor_si_true : valor_si_false
  // El helper setStatusLed aplica automáticamente la polaridad declarada
  // en config.h.
  setStatusLed(relayState == WATERING);
}

// ================================================================
// HANDLERS DEL SERVIDOR WEB
// ================================================================
// Un "handler" (manejador) es una función que el servidor llama
// automáticamente cuando llega una petición HTTP a una ruta específica.
// Se registran en setup() con server.on("/ruta", funcion).

// ── GET /  →  Página HTML de monitoreo ──────────────────────────
// Devuelve una página web completa con el estado actual del sistema.
// El navegador puede recargar manualmente para ver datos actualizados.
// (Para actualización automática se podría agregar meta refresh o JS)
void handleRoot() {
  // ── Determinar el estado legible en español ──────────────────
  // Convertimos el estado interno (enum) a texto para mostrarlo en la web.
  String estado;
  if      (relayState == WATERING) estado = "REGANDO";
  else if (relayState == COOLDOWN) estado = "COOLDOWN";
  else if (lastPercent < ON_THRESHOLD_PERCENT) estado = "SECO";
  else    estado = "HUMEDO";

  // ── Calcular cuándo fue el último riego ──────────────────────
  String ultimoRiego;
  if (lastWaterEndMs == 0) {
    // Nunca ha regado desde el arranque
    ultimoRiego = "Sin riego registrado";
  } else {
    // Convertimos ms a segundos y luego a formato legible
    unsigned long segs = (millis() - lastWaterEndMs) / 1000UL;
    if (segs < 60) {
      ultimoRiego = String(segs) + " seg";
    } else if (segs < 3600) {
      ultimoRiego = String(segs / 60) + " min " + String(segs % 60) + " seg";
    } else {
      ultimoRiego = String(segs / 3600) + " h " + String((segs % 3600) / 60) + " min";
    }
  }

  // ── Construir el HTML ─────────────────────────────────────────
  // Arduino trabaja bien concatenando String. Para proyectos más grandes
  // sería mejor usar PROGMEM o LittleFS, pero para esta escala está bien.
  String html =
    "<!DOCTYPE html>"
    "<html lang='es'><head>"
    "<meta charset='UTF-8'>"
    // viewport: hace que la página se vea bien en celulares
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Humedad Suelo</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:480px;margin:2rem auto;padding:0 1rem}"
    "h1{color:#2a7a2a}"
    ".card{background:#f4f4f4;border-radius:8px;padding:1rem;margin:.5rem 0}"
    ".val{font-size:2rem;font-weight:bold;color:#1a5c1a}"
    "</style>"
    "</head><body>"
    "<h1>🌱 Monitor de Humedad</h1>"
    // String(lastPercent, 1) → número con 1 decimal (ej: "65.3")
    "<div class='card'><div class='val'>" + String(lastPercent, 1) + " %</div>"
    "<div>Humedad del suelo</div></div>"
#if ENABLE_AMBIENT_SENSOR
    "<div class='card'><div class='val'>" + (isnan(lastAmbientTempC) ? String("--") : String(lastAmbientTempC, 1)) + " C</div>"
    "<div>Temperatura ambiental</div></div>"
    "<div class='card'><div class='val'>" + (isnan(lastAmbientHumPct) ? String("--") : String(lastAmbientHumPct, 1)) + " %</div>"
    "<div>Humedad ambiental</div></div>"
#endif
    "<p><a href='/json'>Ver JSON</a></p>"
    "</body></html>";

  // Enviar la respuesta HTTP 200 OK con el HTML
  // "text/html; charset=UTF-8" es el Content-Type para páginas web
  server.send(200, "text/html; charset=UTF-8", html);
}

// ── GET /json  →  Datos en formato JSON ─────────────────────────
// Útil para dashboards, scripts, o cualquier sistema que consuma datos
// del dispositivo directamente por HTTP sin pasar por MQTT.
// Ejemplo de respuesta:
//   {"raw":450,"percent":51.5,"watering":false,"cooldown":false,
//    "state":"WET","last_watered_sec":120}
void handleJson() {
  // Estado en inglés (estándar para APIs/JSON)
  String estado;
  if      (relayState == WATERING) estado = "WATERING";
  else if (relayState == COOLDOWN) estado = "COOLDOWN";
  else if (lastPercent < ON_THRESHOLD_PERCENT) estado = "DRY";
  else    estado = "WET";

  // secsAgo: segundos desde el último riego. -1 si nunca ha regado.
  long secsAgo = (lastWaterEndMs == 0) ? -1L : (long)((millis() - lastWaterEndMs) / 1000UL);

  // Construir el JSON manualmente (sin librería externa para ahorrar RAM)
  String json = "{";
  json.reserve(196);
  json += "\"percent\":";
  json += String(lastPercent, 1);
  json += ",\"watering\":";
  json += (relayState == WATERING ? "true" : "false");
  json += ",\"state\":\"";
  json += estado;
  json += "\",\"last_watered_sec\":";
  json += String(secsAgo);
  json += ",\"on_threshold_percent\":";
  json += String(ON_THRESHOLD_PERCENT);
  json += ",\"relay_on_time_s\":";
  json += String((float)RELAY_ON_TIME_MS / 1000.0f, 1);
#if ENABLE_AMBIENT_SENSOR
  json += ",\"temperature\":";
  if (isnan(lastAmbientTempC)) json += "null";
  else json += String(lastAmbientTempC, 1);
  json += ",\"humidity\":";
  if (isnan(lastAmbientHumPct)) json += "null";
  else json += String(lastAmbientHumPct, 1);
#endif
  json += "}";

  if (json.indexOf("\"last_watered_sec\":") == -1) {
    Serial.printf("[MQTT][WARN] JSON sin last_watered_sec: %s\n", json.c_str());
  }

  // "application/json" es el Content-Type estándar para APIs REST
  server.send(200, "application/json", json);
}

// ================================================================
// mqttCallback(topic, payload, length)  —  Receptor de comandos MQTT
// ================================================================
// PubSubClient llama automáticamente a esta función cada vez que llega
// un mensaje en alguno de los tópicos suscritos. Se registra con
// mqtt.setCallback(mqttCallback) en setup().
//
// PARÁMETROS:
//   topic   : cadena C con el nombre del tópico del mensaje recibido
//             (ej: "commands/esp32_01")
//   payload : array de bytes con el contenido del mensaje.
//             ⚠ NO tiene terminador '\0', no es un string directamente.
//   length  : número de bytes válidos en payload.
//
// FLUJO:
//   1. Copiar payload a un buffer con '\0' al final (para usarlo como string).
//   2. Filtrar: solo procesar mensajes del tópico MQTT_TOPICO_CMD.
//   3. Buscar la acción "water" en el JSON recibido.
//   4. Si el sistema está en IDLE → activar riego.
//      Si ya está regando/enfriando → ignorar (seguridad).
//
// NOTA: No parseamos el JSON con una librería completa para ahorrar RAM.
//       strstr() busca la subcadena "\"water\"" dentro del mensaje,
//       lo cual es suficiente para este formato simple y conocido.
// ================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // ── Paso 1: Convertir payload (bytes sin '\0') a string C ────
  // Declaramos un array local con espacio para el terminador '\0'
  char msg[length + 1];
  memcpy(msg, payload, length);  // Copiar los bytes del payload
  msg[length] = '\0';            // Agregar terminador de string

  Serial.printf("[MQTT] Mensaje recibido en %s: %s\n", topic, msg);

  // ── Paso 2: Filtrar por tópico ───────────────────────────────
  // strcmp compara dos strings C y retorna 0 si son iguales.
  // Si el mensaje NO es del tópico de comandos, salir sin hacer nada.
  if (strcmp(topic, MQTT_TOPICO_CMD) != 0) return;

  // ── Paso 3: Buscar la acción "water" en el JSON ──────────────
  // strstr(cadena, subcadena) retorna un puntero si la encuentra, nullptr si no.
  // Buscamos "\"water\"" (con comillas escapadas) para evitar falsos positivos.
  if (strstr(msg, "\"water\"") != nullptr) {

    // ── Paso 4: Activar riego si el sistema está en reposo ─────
    if (relayState == IDLE) {
      digitalWrite(PIN_RELAY, HIGH);  // HIGH → relé activo → válvula abierta
      setStatusLed(true);
      relayStartMs = millis();        // Registrar inicio del riego
      relayState   = WATERING;        // Transición a estado WATERING
      Serial.println("[MQTT] Comando 'water' recibido. Riego iniciado.");
    } else {
      // El sistema ya está regando o en cooldown → ignorar el comando
      // (seguridad: evita extender el riego más allá de RELAY_ON_TIME_MS)
      Serial.println("[MQTT] Comando 'water' recibido pero el sistema no está en IDLE; ignorado.");
    }
  }
}

// ================================================================
// reconnectMQTT()  —  Conexión y reconexión al broker MQTT
// ================================================================
// MQTT requiere una conexión TCP persistente al broker.
// Si la red se cae o el broker se reinicia, la conexión se pierde.
// Esta función se encarga de (re)establecerla cuando sea necesario.
//
// ¿POR QUÉ NO USAMOS connect() DIRECTAMENTE EN loop()?
//   mqtt.connect() puede tardar varios segundos en fallar (timeout TCP).
//   Si lo llamáramos en cada iteración del loop() cuando no hay conexión,
//   bloquearíamos el procesador y el servidor web dejaría de responder.
//   En su lugar, controlamos el intervalo con lastMqttAttemptMs y solo
//   llamamos a reconnectMQTT() cada MQTT_RECONNECT_INTERVAL_MS (5 s).
//
// ¿POR QUÉ SUSCRIBIRSE AQUÍ Y NO EN setup()?
//   En MQTT, las suscripciones se pierden al desconectarse del broker.
//   Cada vez que reconectamos debemos volver a suscribirnos.
//   Por eso la llamada a mqtt.subscribe() está dentro del if(ok) y no
//   en un lugar que se ejecute una sola vez.
//
// RETORNO:
//   true  → el cliente está conectado al finalizar (ya estaba o conectó OK)
//   false → MQTT deshabilitado (MQTT_SERVER vacío) o falló la conexión
// ================================================================
bool reconnectMQTT() {
  // Si MQTT_SERVER está vacío en config.h → MQTT deshabilitado, nada que hacer
  if (strlen(MQTT_SERVER) == 0) return false;

  // Si ya está conectado, no hacer nada (la reconexión es innecesaria)
  if (mqtt.connected()) return true;

  Serial.print("[MQTT] Conectando a ");
  Serial.print(MQTT_SERVER);
  Serial.print("...");

  // mqtt.connect() intenta la conexión TCP + handshake MQTT.
  // Si el broker requiere usuario/contraseña, usamos la versión con credenciales.
  // strlen() == 0 significa cadena vacía → sin autenticación.
  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqtt.connect(
      mqttClientIdDynamic,
      MQTT_USER,
      MQTT_PASS_BROKER,
      MQTT_STATUS_TOPIC,
      1,
      true,
      "offline"
    );
  } else {
    ok = mqtt.connect(
      mqttClientIdDynamic,
      MQTT_STATUS_TOPIC,
      1,
      true,
      "offline"
    );  // Conexión anónima (sin usuario/contraseña)
  }

  if (ok) {
    Serial.println(" OK");

    // Suscribirse al tópico de comandos para recibir órdenes remotas.
    // QoS 0 (por defecto en PubSubClient): fire-and-forget, sin confirmación.
    // Suficiente para comandos de riego donde la latencia importa poco.
    mqtt.subscribe(MQTT_TOPICO_CMD);
    Serial.printf("[MQTT] Suscrito a %s\n", MQTT_TOPICO_CMD);

    // Marcar presencia online con mensaje retained para observabilidad.
    mqtt.publish(MQTT_STATUS_TOPIC, "online", true);
    Serial.printf("[MQTT] Presencia online publicada en %s\n", MQTT_STATUS_TOPIC);

    // Publicar inmediatamente la última lectura para evitar "silencio"
    // tras reconexión o reinicio (no esperar BACKGROUND_SAMPLE_MS).
    publicarMQTT();
  } else {
    // mqtt.state() retorna un código de error numérico:
    //  -4: MQTT_CONNECTION_TIMEOUT   -3: MQTT_CONNECTION_LOST
    //  -2: MQTT_CONNECT_FAILED       -1: MQTT_DISCONNECTED
    //   1: MQTT_CONNECT_BAD_PROTOCOL  2: MQTT_CONNECT_BAD_CLIENT_ID
    //   3: MQTT_CONNECT_UNAVAILABLE   4: MQTT_CONNECT_BAD_CREDENTIALS
    //   5: MQTT_CONNECT_UNAUTHORIZED
    Serial.print(" FALLO (rc=");
    Serial.print(mqtt.state());
    Serial.println(")");
  }
  return ok;
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;

  unsigned long now = millis();
  if (now - lastWifiAttemptMs < WIFI_RECONNECT_INTERVAL_MS) return false;

  lastWifiAttemptMs = now;
  Serial.println("[WIFI] Desconectado. Reintentando conexión...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  return false;
}

// ================================================================
// publicarMQTT()  —  Envío de datos del sensor al broker
// ================================================================
// Publica el estado completo del sistema en el tópico MQTT_TOPICO.
// La Raspberry Pi (con mqtt_client.py corriendo) recibe este mensaje,
// lo parsea y lo almacena en la base de datos para dashboards.
//
// FORMATO DEL JSON PUBLICADO:
//   {
//     "percent"             : 51.5,  ← humedad en % (0.0–100.0)
//     "watering"            : false, ← true si la válvula está abierta ahora
//     "state"               : "WET", ← "DRY", "WET", "WATERING" o "COOLDOWN"
//     "last_watered_sec"    : 120,   ← segundos desde el último riego (-1 nunca)
//     "on_threshold_percent": 35,    ← umbral (%) que activa el relé
//     "relay_on_time_s"     : 1.0    ← duración de cada ciclo de riego en segundos
//   }
//
// ¿QUÉ ES QoS EN MQTT?
//   Quality of Service: nivel de garantía de entrega del mensaje.
//   mqtt.publish() usa QoS 0 por defecto: el mensaje se envía una vez,
//   sin confirmación. Si el broker no lo recibe, se pierde. Para datos
//   de sensor periódicos esto es aceptable (viene otro en BACKGROUND_SAMPLE_MS).
// ================================================================
void publicarMQTT() {
  // No hacer nada si no hay conexión activa al broker
  if (!mqtt.connected()) return;

  // Determinar el estado del sistema en texto
  String estado;
  if      (relayState == WATERING) estado = "WATERING";
  else if (relayState == COOLDOWN) estado = "COOLDOWN";
  else if (lastPercent < ON_THRESHOLD_PERCENT) estado = "DRY";
  else    estado = "WET";
  long secsAgo = (lastWaterEndMs == 0) ? -1L : (long)((millis() - lastWaterEndMs) / 1000UL);

  // Construir el JSON como String de Arduino
  // c_str() convierte String de Arduino a cadena C (const char*) que
  // necesita mqtt.publish()
  String json = "{";
  json += "\"percent\":"              + String(lastPercent, 1)                         + ",";
  json += "\"watering\":"             + String(relayState == WATERING ? "true":"false") + ",";
  json += "\"state\":\""              + estado                                         + "\",";
  json += "\"last_watered_sec\":"     + String(secsAgo)                                + ",";
  json += "\"on_threshold_percent\":" + String(ON_THRESHOLD_PERCENT)                   + ",";
  json += "\"relay_on_time_s\":"      + String((float)RELAY_ON_TIME_MS / 1000.0f, 1);
#if ENABLE_AMBIENT_SENSOR
  if (!isnan(lastAmbientTempC)) {
    json += ",\"temperature\":" + String(lastAmbientTempC, 1);
  }
  if (!isnan(lastAmbientHumPct)) {
    json += ",\"humidity\":" + String(lastAmbientHumPct, 1);
  }
#endif
  json += "}";

  // mqtt.publish(topico, mensaje) retorna true si el mensaje fue encolado OK
  bool publicado = mqtt.publish(MQTT_TOPICO, json.c_str());
  Serial.printf("[MQTT] Publicado en %s: %s (%s)\n",
                MQTT_TOPICO, json.c_str(), publicado ? "OK" : "FALLO");
}

// ================================================================
// setup()  —  Inicialización del sistema (se ejecuta UNA SOLA VEZ)
// ================================================================
// En Arduino, setup() es la función de arranque. Se ejecuta una vez
// al encender o resetear el microcontrolador. Aquí configuramos todo antes
// de entrar al bucle principal (loop()).
// ================================================================
void setup() {
  // ── Monitor Serie ─────────────────────────────────────────────
  // Inicia la comunicación serial a 115200 baudios.
  // Esto nos permite ver mensajes de depuración en el Monitor Serie
  // de Arduino IDE (Herramientas → Monitor Serie → 115200 baud).
  Serial.begin(115200);
  Serial.println("\n[INICIO] humedadSueloK8");

#if defined(ESP32)
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_AO, ADC_11db);
#endif

  // ── Configuración de pines ────────────────────────────────────
  // pinMode() define si un pin es entrada (INPUT) o salida (OUTPUT).
  pinMode(PIN_DO,    INPUT);   // DO del sensor: lectura digital (informativo)
  pinMode(PIN_RELAY, OUTPUT);  // Control del relé: salida digital
  if (PIN_LED >= 0) {
    pinMode(PIN_LED, OUTPUT);  // LED integrado: salida digital
  }

#if ENABLE_AMBIENT_SENSOR
  pinMode(PIN_AMBIENT, INPUT_PULLUP);
  ambientSensor.begin();
  delay(2000);
  Serial.printf("[AMBIENT] Sensor DHT iniciado en GPIO %d\n", PIN_AMBIENT);
#endif

  // ── Estado seguro al arranque ─────────────────────────────────
  // Al arrancar siempre ponemos el relé inactivo para evitar que la
  // válvula quede abierta por un reinicio inesperado del dispositivo.
  digitalWrite(PIN_RELAY, LOW);   // LOW → relé inactivo → válvula CERRADA (seguro)
  setStatusLed(false);

  // ── Conexión Wi-Fi ────────────────────────────────────────────
  // WiFi.begin() inicia el proceso de conexión en segundo plano.
  // Esperamos hasta WIFI_BOOT_TIMEOUT_MS para no bloquear indefinidamente.
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WIFI] Conectando");
  unsigned long wifiBootStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiBootStart < WIFI_BOOT_TIMEOUT_MS)) {
    delay(500);
    Serial.print(".");  // Imprime un punto cada 500ms como indicador de progreso
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("[WIFI] Conectado. IP: ");
    Serial.println(WiFi.localIP());  // La IP local para acceder al servidor web
  } else {
    Serial.println();
    Serial.println("[WIFI] Timeout inicial. El loop seguirá reintentando.");
  }

  // ── Servidor web ──────────────────────────────────────────────
  // server.on() asocia una URL con su función manejadora (handler).
  // server.begin() pone el servidor en escucha en el puerto 80.
  server.on("/",     handleRoot);  // GET / → página HTML
  server.on("/json", handleJson);  // GET /json → datos JSON
  server.begin();
  Serial.println("[WEB] Servidor iniciado en puerto 80");

  // ── Primera lectura del sensor ────────────────────────────────
  // Hacemos una lectura inicial para que el servidor web tenga datos
  // reales desde el primer momento (en lugar de mostrar 0%).
  lastRaw      = readADC();
  lastPercent  = rawToPercent(lastRaw);
  lastSampleMs = millis();  // Registrar el instante de esta primera lectura
  Serial.printf("[ADC] Raw: %d | Humedad: %.1f%%\n", lastRaw, lastPercent);

#if ENABLE_AMBIENT_SENSOR
  float t0, h0;
  bool okAmbient = readAmbient(t0, h0);
  if (okAmbient) {
    lastAmbientTempC = t0;
    lastAmbientHumPct = h0;
  }
  lastAmbientSampleMs = millis();
  if (!okAmbient) {
    Serial.println("[AMBIENT] Lectura inicial no disponible.");
  } else {
    Serial.printf("[AMBIENT] Temp: %.1f C | Humedad: %.1f%%\n", lastAmbientTempC, lastAmbientHumPct);
  }
#endif

  // ── MQTT ──────────────────────────────────────────────────────
  // Solo configuramos MQTT si el usuario definió un servidor en config.h.
  // Si MQTT_SERVER es "" → toda la lógica MQTT se omite silenciosamente.
  if (strlen(MQTT_SERVER) > 0) {
    // Reservar espacio para "-<chipid-6hex>" + '\0' (8 chars) y usar el resto
    // para el prefijo configurable del clientId.
    const int prefixMaxLen = (int)sizeof(mqttClientIdDynamic) - 8;
    snprintf(
      mqttClientIdDynamic,
      sizeof(mqttClientIdDynamic),
      "%.*s-%06X",
      prefixMaxLen,
      MQTT_CLIENT_ID,
      (unsigned long)getDeviceIdSuffix()
    );
    Serial.printf("[MQTT] ClientId dinámico: %s\n", mqttClientIdDynamic);

    mqtt.setServer(MQTT_SERVER, MQTT_PORT);   // IP y puerto del broker
    mqtt.setCallback(mqttCallback);           // Función que recibirá los mensajes entrantes
    reconnectMQTT();                          // Primer intento de conexión
  }
}

// ================================================================
// loop()  —  Bucle principal (se ejecuta CONTINUAMENTE)
// ================================================================
// En Arduino, loop() es el corazón del programa. Se llama repetidamente
// sin parar mientras el microcontrolador está encendido.
//
// PRINCIPIO CLAVE: NO BLOQUEANTE
//   Usamos millis() para temporizar tareas en lugar de delay().
//   Esto permite que todas las tareas (web, MQTT, sensor, relé) se
//   ejecuten de forma "paralela" en un solo hilo de ejecución:
//   cada tarea revisa si "le toca actuar" y si no, cede el control.
//
// ORDEN DE EJECUCIÓN EN CADA ITERACIÓN:
//   1. Atender peticiones HTTP entrantes (servidor web)
//   2. Mantener la conexión MQTT viva y procesar mensajes entrantes
//   3. Si pasó BACKGROUND_SAMPLE_MS: leer sensor y publicar por MQTT
//   4. Evaluar la máquina de estados del relé y actualizar el LED
// ================================================================
void loop() {

  // ── 1. Servidor web ───────────────────────────────────────────
  // handleClient() revisa si hay alguna petición HTTP pendiente.
  // Si hay una, la procesa llamando al handler correspondiente (handleRoot
  // o handleJson). Si no hay nada, retorna inmediatamente sin bloquear.
  server.handleClient();

  // ── 2. MQTT: mantener conexión y procesar mensajes entrantes ──
  if (strlen(MQTT_SERVER) > 0) {
    ensureWiFiConnected();

    unsigned long now = millis();

    // Si la conexión se perdió, intentar reconectar cada 5 segundos.
    // Usamos el patrón "non-blocking retry" con timestamp:
    //   - Guardamos cuándo fue el último intento (lastMqttAttemptMs)
    //   - Solo reintentamos si pasaron >= MQTT_RECONNECT_INTERVAL_MS ms
    if (WiFi.status() == WL_CONNECTED &&
        !mqtt.connected() &&
        (now - lastMqttAttemptMs >= MQTT_RECONNECT_INTERVAL_MS)) {
      lastMqttAttemptMs = now;
      reconnectMQTT();
    }

    // mqtt.loop() es OBLIGATORIO en cada iteración cuando se usa PubSubClient.
    // Procesa los mensajes entrantes (llama a mqttCallback si llegó algo)
    // y envía los keepalive MQTT para que el broker no cierre la conexión.
    if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
      mqtt.loop();
    }
  }

  // ── 3. Lectura periódica del sensor y publicación MQTT ────────
  unsigned long now = millis();

  // Patrón "temporizador no bloqueante": verificamos si transcurrió
  // el intervalo deseado comparando el tiempo actual con la última lectura.
  // BACKGROUND_SAMPLE_MS está definido en config.h (por defecto 3000 ms).
  if (now - lastSampleMs >= BACKGROUND_SAMPLE_MS) {
    lastSampleMs = now;              // Actualizar el momento de la última lectura

    lastRaw     = readADC();         // Leer el ADC con promediado
    lastPercent = rawToPercent(lastRaw);  // Convertir a porcentaje de humedad

    Serial.printf("[ADC] Raw: %d | Humedad: %.1f%%\n", lastRaw, lastPercent);

    // Publicar los datos actuales al broker MQTT para que la Raspberry Pi
    // los almacene en la base de datos y los muestre en el dashboard.
    publicarMQTT();
  }

#if ENABLE_AMBIENT_SENSOR
  if (now - lastAmbientSampleMs >= AMBIENT_SAMPLE_MS) {
    lastAmbientSampleMs = now;

    float t, h;
    bool okAmbient = readAmbient(t, h);
    if (!okAmbient) {
      Serial.println("[AMBIENT] Lectura invalida (NaN).");
    } else {
      lastAmbientTempC = t;
      lastAmbientHumPct = h;
      Serial.printf("[AMBIENT] Temp: %.1f C | Humedad: %.1f%%\n", t, h);
    }
  }
#endif

  // ── 4. Control del relé y LED según el estado de humedad ──────
  // updateRelay() evalúa la máquina de estados con el porcentaje actual
  // y decide si debe abrir/cerrar la válvula y encender/apagar el LED.
  // Se llama en CADA iteración del loop() (no solo cuando hay nueva lectura)
  // para que las transiciones de estado (ej: fin del tiempo de riego)
  // se detecten con precisión temporal, sin esperar al siguiente muestreo.
  updateRelay(lastPercent);
}
