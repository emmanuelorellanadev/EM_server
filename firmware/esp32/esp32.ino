/*
  Firmware de humedad de suelo con control de riego, web local y MQTT.

  Flujo principal:
  - Lee sensores: suelo (ADC), ambiental DHT (opcional) y luz LDR (opcional).
  - Controla relé por umbral y máquina de estados (IDLE/WATERING/COOLDOWN).
  - Publica telemetría agregada por ventana en MQTT.
  - Acepta comando remoto {"action":"water"} por MQTT.

  Configuración: editar config.h y recompilar.
  Documentación funcional: CONFIGURACION.md
*/

#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include "config.h"
#include <math.h>

#if MQTT_WINDOW_SAMPLE_COUNT <= 0
#error "MQTT_WINDOW_SAMPLE_COUNT debe ser mayor que 0"
#endif

#if AGGREGATION_SAMPLE_MS <= 0
#error "AGGREGATION_SAMPLE_MS debe ser mayor que 0"
#endif

#if MQTT_PUBLISH_INTERVAL_MS <= 0
#error "MQTT_PUBLISH_INTERVAL_MS debe ser mayor que 0"
#endif

#if MQTT_PUBLISH_INTERVAL_MS < AGGREGATION_SAMPLE_MS
#error "MQTT_PUBLISH_INTERVAL_MS debe ser >= AGGREGATION_SAMPLE_MS"
#endif

#if ((MQTT_PUBLISH_INTERVAL_MS % AGGREGATION_SAMPLE_MS) != 0)
#error "MQTT_PUBLISH_INTERVAL_MS debe ser multiplo de AGGREGATION_SAMPLE_MS"
#endif

#if ((MQTT_PUBLISH_INTERVAL_MS / AGGREGATION_SAMPLE_MS) != MQTT_WINDOW_SAMPLE_COUNT)
#error "MQTT_WINDOW_SAMPLE_COUNT debe ser igual a MQTT_PUBLISH_INTERVAL_MS / AGGREGATION_SAMPLE_MS"
#endif

#if ENABLE_AMBIENT_SENSOR
#include <DHT.h>
#endif

WebServer server(80);

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

char mqttClientIdDynamic[32] = {0};

unsigned long lastMqttAttemptMs = 0;
unsigned long lastWifiAttemptMs = 0;

enum RelayState {
  IDLE,
  WATERING,
  COOLDOWN
};

RelayState relayState = IDLE;

unsigned long relayStartMs    = 0;
unsigned long cooldownStartMs = 0;
unsigned long lastWaterEndMs  = 0;

// Latest sensor values used by UI, control, and publish routines.

float         lastSoilVwc           = 0.0f;
int           lastRaw               = 0;
unsigned long lastControlSampleMs   = 0;

// Fixed-size buffers for MQTT aggregation window.
float         soilVwcWindow[MQTT_WINDOW_SAMPLE_COUNT] = {0};
int           rawWindow[MQTT_WINDOW_SAMPLE_COUNT] = {0};
#if ENABLE_AMBIENT_SENSOR
float         ambientTempWindow[MQTT_WINDOW_SAMPLE_COUNT] = {0};
float         ambientHumWindow[MQTT_WINDOW_SAMPLE_COUNT] = {0};
bool          ambientValidWindow[MQTT_WINDOW_SAMPLE_COUNT] = {false};
#endif
#if ENABLE_LIGHT_SENSOR
int           lightRawWindow[MQTT_WINDOW_SAMPLE_COUNT] = {0};
float         lightPercentWindow[MQTT_WINDOW_SAMPLE_COUNT] = {0};
#endif
uint8_t       windowWriteIndex = 0;
uint8_t       windowSampleCount = 0;
unsigned long lastAggregationSampleMs = 0;
unsigned long lastMqttPublishMs = 0;
bool          bootstrapTelemetryPublished = false;

float         lastAmbientTempC    = NAN;
float         lastAmbientHumPct   = NAN;

#if ENABLE_LIGHT_SENSOR
int           lastLightRaw        = 0;
float         lastLightPercent    = 0.0f;
#endif

struct AggregatedSnapshot {
  float soil_vwc = 0.0f;
  int raw = 0;
  uint8_t samples = 0;
#if ENABLE_AMBIENT_SENSOR
  float temperature = NAN;
  float humidity = NAN;
  uint8_t ambientSamples = 0;
#endif
#if ENABLE_LIGHT_SENSOR
  int lightRaw = 0;
  float lightPercent = 0.0f;
  uint8_t lightSamples = 0;
#endif
};

#if ENABLE_AMBIENT_SENSOR
#if AMBIENT_SENSOR_DHT22
#define AMBIENT_SENSOR_TYPE DHT22
#else
#define AMBIENT_SENSOR_TYPE DHT11
#endif
DHT ambientSensor(PIN_AMBIENT, AMBIENT_SENSOR_TYPE);
bool readAmbient(float& outTempC, float& outHumPct);
#endif

#if ENABLE_LIGHT_SENSOR
int   readLight();
float rawToLightPercent(int raw);
#endif

void pushWindowSample(int raw, float soil_vwc, bool ambientOk, float ambientTemp, float ambientHum
#if ENABLE_LIGHT_SENSOR
                      , int lightRaw, float lightPercent
#endif
                     ) {
  const uint8_t idx = windowWriteIndex;
  rawWindow[idx] = raw;
  soilVwcWindow[idx] = soil_vwc;
#if ENABLE_AMBIENT_SENSOR
  ambientValidWindow[idx] = ambientOk;
  if (ambientOk) {
    ambientTempWindow[idx] = ambientTemp;
    ambientHumWindow[idx] = ambientHum;
  }
#endif
#if ENABLE_LIGHT_SENSOR
  lightRawWindow[idx] = lightRaw;
  lightPercentWindow[idx] = lightPercent;
#endif

  windowWriteIndex = (uint8_t)((windowWriteIndex + 1U) % MQTT_WINDOW_SAMPLE_COUNT);
  if (windowSampleCount < MQTT_WINDOW_SAMPLE_COUNT) {
    windowSampleCount++;
  }
}

void resetWindowSamples() {
  windowSampleCount = 0;
  windowWriteIndex = 0;
  for (uint8_t i = 0; i < MQTT_WINDOW_SAMPLE_COUNT; i++) {
    soilVwcWindow[i] = 0.0f;
    rawWindow[i] = 0;
#if ENABLE_AMBIENT_SENSOR
    ambientValidWindow[i] = false;
    ambientTempWindow[i] = 0.0f;
    ambientHumWindow[i] = 0.0f;
#endif
#if ENABLE_LIGHT_SENSOR
    lightRawWindow[i] = 0;
    lightPercentWindow[i] = 0.0f;
#endif
  }
}

bool buildAggregatedSnapshot(AggregatedSnapshot& out) {
  if (windowSampleCount == 0) {
    return false;
  }

  float sumSoilVwc = 0.0f;
  long sumRaw = 0;
#if ENABLE_AMBIENT_SENSOR
  float sumAmbientTemp = 0.0f;
  float sumAmbientHum = 0.0f;
  uint8_t ambientCount = 0;
#endif
#if ENABLE_LIGHT_SENSOR
  long sumLightRaw = 0;
  float sumLightPct = 0.0f;
#endif

  for (uint8_t i = 0; i < windowSampleCount; i++) {
    sumSoilVwc += soilVwcWindow[i];
    sumRaw += rawWindow[i];
#if ENABLE_AMBIENT_SENSOR
    if (ambientValidWindow[i]) {
      sumAmbientTemp += ambientTempWindow[i];
      sumAmbientHum += ambientHumWindow[i];
      ambientCount++;
    }
#endif
#if ENABLE_LIGHT_SENSOR
    sumLightRaw += lightRawWindow[i];
    sumLightPct += lightPercentWindow[i];
#endif
  }

  out.samples = windowSampleCount;
  out.soil_vwc = sumSoilVwc / (float)windowSampleCount;
  out.raw = (int)(sumRaw / (long)windowSampleCount);

#if ENABLE_AMBIENT_SENSOR
  out.ambientSamples = ambientCount;
  if (ambientCount > 0) {
    out.temperature = sumAmbientTemp / (float)ambientCount;
    out.humidity = sumAmbientHum / (float)ambientCount;
  }
#endif

#if ENABLE_LIGHT_SENSOR
  out.lightSamples = windowSampleCount;
  out.lightRaw = (int)(sumLightRaw / (long)windowSampleCount);
  out.lightPercent = sumLightPct / (float)windowSampleCount;
#endif

  return true;
}

void captureAggregationSample() {
  int raw = readADC();
  float soil_vwc = rawToSoilVwc(raw);

  lastRaw = raw;
  lastSoilVwc = soil_vwc;

  bool ambientOk = false;
  float ambientTemp = NAN;
  float ambientHum = NAN;

#if ENABLE_AMBIENT_SENSOR
  ambientOk = readAmbient(ambientTemp, ambientHum);
  if (ambientOk) {
    lastAmbientTempC = ambientTemp;
    lastAmbientHumPct = ambientHum;
  }
#endif

#if ENABLE_LIGHT_SENSOR
  int lightRaw = readLight();
  float lightPercent = rawToLightPercent(lightRaw);
  lastLightRaw = lightRaw;
  lastLightPercent = lightPercent;
#endif

  pushWindowSample(raw, soil_vwc, ambientOk, ambientTemp, ambientHum
#if ENABLE_LIGHT_SENSOR
                   , lightRaw, lightPercent
#endif
                  );
}

#if ENABLE_AMBIENT_SENSOR
bool readAmbient(float& outTempC, float& outHumPct) {
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

#if ENABLE_LIGHT_SENSOR
int readLight() {
  long sum = 0;
  for (int i = 0; i < ANALOG_SAMPLES; i++) {
    sum += analogRead(PIN_LIGHT);
    delay(ANALOG_DELAY_MS);
  }
  return (int)(sum / ANALOG_SAMPLES);
}

float rawToLightPercent(int raw) {
  float pct = (float)(raw - LIGHT_DARK_RAW) / (float)(LIGHT_BRIGHT_RAW - LIGHT_DARK_RAW) * 100.0f;
  if (pct < 0.0f)   pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  return pct;
}
#endif

uint32_t getDeviceIdSuffix() {
  return (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFUL);
}

int readADC() {
  long sum = 0;
  for (int i = 0; i < ANALOG_SAMPLES; i++) {
    sum += analogRead(PIN_AO);
    delay(ANALOG_DELAY_MS);
  }
  return (int)(sum / ANALOG_SAMPLES);
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

float rawToSoilVwc(int raw) {
  float pct = (float)(RAW_DRY - raw) / (float)(RAW_DRY - RAW_WET) * 100.0f;
  if (pct < 0.0f)   pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  return pct;
}

void updateRelay(float pct) {
  unsigned long now = millis();

  switch (relayState) {
    case IDLE:
      if (pct < ON_THRESHOLD_SOIL_VWC) {
        digitalWrite(PIN_RELAY, HIGH);
        relayStartMs = now;
        relayState   = WATERING;
        Serial.printf("[RIEGO] Iniciado. Humedad: %.1f%%\n", pct);
      }
      break;

    case WATERING:
      if (now - relayStartMs >= RELAY_ON_TIME_MS) {
        digitalWrite(PIN_RELAY, LOW);
        lastWaterEndMs  = now;
        cooldownStartMs = now;
        relayState      = COOLDOWN;
        Serial.println("[RIEGO] Terminado. Iniciando cooldown.");
      }
      break;

    case COOLDOWN:
      if (now - cooldownStartMs >= COOLDOWN_MS) {
        relayState = IDLE;
        Serial.println("[RIEGO] Cooldown finalizado. Listo para siguiente ciclo.");
      }
      break;
  }

  setStatusLed(relayState == WATERING);
}

String buildJsonCore(float soil_vwc, int raw) {
  String estado;
  if      (relayState == WATERING) estado = "WATERING";
  else if (relayState == COOLDOWN) estado = "COOLDOWN";
  else if (soil_vwc < ON_THRESHOLD_SOIL_VWC) estado = "DRY";
  else    estado = "WET";

  long secsAgo = (lastWaterEndMs == 0) ? -1L : (long)((millis() - lastWaterEndMs) / 1000UL);

  String json = "{";
  json += "\"soil_humidity\":{\"raw\":" + String(raw);
  json += ",\"unit\":\"%\"";
  json += ",\"value\":" + String(soil_vwc, 1) + "}";
  json += ",\"watering\":"           + String(relayState == WATERING ? "true" : "false");
  json += ",\"state\":\""            + estado + "\"";
  json += ",\"last_watered_sec\":"   + String(secsAgo);
  json += ",\"on_threshold_soil_vwc\":" + String(ON_THRESHOLD_SOIL_VWC);
  json += ",\"relay_on_time_s\":"    + String((float)RELAY_ON_TIME_MS / 1000.0f, 1);
  return json;
}

void handleRoot() {
  String estado;
  if      (relayState == WATERING) estado = "REGANDO";
  else if (relayState == COOLDOWN) estado = "COOLDOWN";
  else if (lastSoilVwc < ON_THRESHOLD_SOIL_VWC) estado = "SECO";
  else    estado = "HUMEDO";

  String ultimoRiego;
  if (lastWaterEndMs == 0) {
    ultimoRiego = "Sin riego registrado";
  } else {
    unsigned long segs = (millis() - lastWaterEndMs) / 1000UL;
    if (segs < 60) {
      ultimoRiego = String(segs) + " seg";
    } else if (segs < 3600) {
      ultimoRiego = String(segs / 60) + " min " + String(segs % 60) + " seg";
    } else {
      ultimoRiego = String(segs / 3600) + " h " + String((segs % 3600) / 60) + " min";
    }
  }

  String html =
    "<!DOCTYPE html>"
    "<html lang='es'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Humedad Suelo</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:480px;margin:2rem auto;padding:0 1rem}"
    "h1{color:#2a7a2a}"
    ".card{background:#f4f4f4;border-radius:8px;padding:1rem;margin:.5rem 0}"
    ".val{font-size:2rem;font-weight:bold;color:#1a5c1a}"
    "</style>"
    "</head><body>"
    "<h1>Monitor de Humedad</h1>"
    "<div class='card'><div class='val'>" + String(lastSoilVwc, 1) + " %</div>"
    "<div>Humedad del suelo</div></div>"
#if ENABLE_AMBIENT_SENSOR
    "<div class='card'><div class='val'>" + (isnan(lastAmbientTempC) ? String("--") : String(lastAmbientTempC, 1)) + " C</div>"
    "<div>Temperatura ambiental</div></div>"
    "<div class='card'><div class='val'>" + (isnan(lastAmbientHumPct) ? String("--") : String(lastAmbientHumPct, 1)) + " %</div>"
    "<div>Humedad ambiental</div></div>"
#endif
#if ENABLE_LIGHT_SENSOR
    "<div class='card'><div class='val'>" + String(lastLightPercent, 1) + " %</div>"
    "<div>Luz ambiental</div></div>"
#endif
    "<p><a href='/json'>Ver JSON</a></p>"
    "</body></html>";

  server.send(200, "text/html; charset=UTF-8", html);
}

void handleJson() {
  String json = buildJsonCore(lastSoilVwc, lastRaw);
#if ENABLE_AMBIENT_SENSOR
  json += ",\"ambient_temperature\":";
  if (isnan(lastAmbientTempC)) json += "null";
  else json += "{\"unit\":\"°C\",\"value\":" + String(lastAmbientTempC, 1) + "}";
  json += ",\"ambient_humidity\":";
  if (isnan(lastAmbientHumPct)) json += "null";
  else json += "{\"unit\":\"%\",\"value\":" + String(lastAmbientHumPct, 1) + "}";
#endif
#if ENABLE_LIGHT_SENSOR
  json += ",\"light\":";
  json += "{\"raw\":" + String(lastLightRaw) + ",\"unit\":\"%\",\"value\":" + String(lastLightPercent, 1) + "}";
#endif
  json += "}";

  if (json.indexOf("\"last_watered_sec\":") == -1) {
    Serial.printf("[HTTP][WARN] JSON sin last_watered_sec: %s\n", json.c_str());
  }

  server.send(200, "application/json", json);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[length + 1];
  memcpy(msg, payload, length);
  msg[length] = '\0';

  Serial.printf("[MQTT] Mensaje recibido en %s: %s\n", topic, msg);

  if (strcmp(topic, MQTT_TOPICO_CMD) != 0) return;

  if (strstr(msg, "\"water\"") != nullptr) {
    if (relayState == IDLE) {
      digitalWrite(PIN_RELAY, HIGH);
      setStatusLed(true);
      relayStartMs = millis();
      relayState   = WATERING;
      Serial.println("[MQTT] Comando 'water' recibido. Riego iniciado.");
    } else {
      Serial.println("[MQTT] Comando 'water' recibido pero el sistema no esta en IDLE; ignorado.");
    }
  }
}

bool reconnectMQTT() {
  if (strlen(MQTT_SERVER) == 0) return false;

  if (mqtt.connected()) return true;

  Serial.print("[MQTT] Conectando a ");
  Serial.print(MQTT_SERVER);
  Serial.print("...");

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
    );
  }

  if (ok) {
    Serial.println(" OK");

    mqtt.subscribe(MQTT_TOPICO_CMD);
    Serial.printf("[MQTT] Suscrito a %s\n", MQTT_TOPICO_CMD);

    mqtt.publish(MQTT_STATUS_TOPIC, "online", true);
    Serial.printf("[MQTT] Presencia online publicada en %s\n", MQTT_STATUS_TOPIC);
  } else {
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
  Serial.println("[WIFI] Desconectado. Reintentando conexion...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  return false;
}

bool publicarMQTT() {
  if (!mqtt.connected()) return false;

  AggregatedSnapshot snap;
  if (!buildAggregatedSnapshot(snap)) {
    Serial.println("[MQTT] Sin muestras en ventana; publicacion omitida.");
    return false;
  }

  String json = buildJsonCore(snap.soil_vwc, snap.raw);
  json += ",\"window_samples\":" + String((int)snap.samples);
#if ENABLE_AMBIENT_SENSOR
  if (!isnan(snap.temperature)) {
    json += ",\"ambient_temperature\":{\"unit\":\"°C\",\"value\":" + String(snap.temperature, 1) + "}";
  }
  if (!isnan(snap.humidity)) {
    json += ",\"ambient_humidity\":{\"unit\":\"%\",\"value\":" + String(snap.humidity, 1) + "}";
  }
  json += ",\"ambient_window_samples\":" + String((int)snap.ambientSamples);
#endif
#if ENABLE_LIGHT_SENSOR
  json += ",\"light\":{\"raw\":" + String(snap.lightRaw);
  json += ",\"unit\":\"%\"";
  json += ",\"value\":" + String(snap.lightPercent, 1) + "}";
#endif
  json += "}";

  bool publicado = mqtt.publish(MQTT_TOPICO, json.c_str());
  Serial.printf("[MQTT] Publicado en %s: %s (%s)\n",
                MQTT_TOPICO, json.c_str(), publicado ? "OK" : "FALLO");
  if (!publicado) {
    return false;
  }

  resetWindowSamples();
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n[INICIO] humedadSueloK8");

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_AO, ADC_11db);

  pinMode(PIN_DO,    INPUT);
  pinMode(PIN_RELAY, OUTPUT);
  if (PIN_LED >= 0) {
    pinMode(PIN_LED, OUTPUT);
  }

#if ENABLE_AMBIENT_SENSOR
  pinMode(PIN_AMBIENT, INPUT_PULLUP);
  ambientSensor.begin();
  delay(2000);
  Serial.printf("[AMBIENT] Sensor DHT iniciado en GPIO %d\n", PIN_AMBIENT);
#endif

  digitalWrite(PIN_RELAY, LOW);
  setStatusLed(false);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WIFI] Conectando");
  unsigned long wifiBootStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiBootStart < WIFI_BOOT_TIMEOUT_MS)) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("[WIFI] Conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("[WIFI] Timeout inicial. El loop seguira reintentando.");
  }

  server.on("/",     handleRoot);
  server.on("/json", handleJson);
  server.begin();
  Serial.println("[WEB] Servidor iniciado en puerto 80");

  captureAggregationSample();
  lastControlSampleMs = millis();
  lastAggregationSampleMs = lastControlSampleMs;
  lastMqttPublishMs = lastControlSampleMs;

  if (strlen(MQTT_SERVER) > 0) {
    const int prefixMaxLen = (int)sizeof(mqttClientIdDynamic) - 8;
    snprintf(
      mqttClientIdDynamic,
      sizeof(mqttClientIdDynamic),
      "%.*s-%06X",
      prefixMaxLen,
      MQTT_CLIENT_ID,
      (unsigned long)getDeviceIdSuffix()
    );
    Serial.printf("[MQTT] ClientId dinamico: %s\n", mqttClientIdDynamic);

    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    // Aumenta el buffer interno de PubSubClient para que quepa el payload
    // completo con todos los sensores (suelo + DHT + LDR). El default de 256
    // bytes queda justo o corto con los objetos anidados de sensores.
    mqtt.setBufferSize(512);
    reconnectMQTT();

    // Publicacion bootstrap: espera a que mqtt.loop() procese el CONNACK
    // y vacie el buffer TX antes de enviar la primera telemetria.
    if (!bootstrapTelemetryPublished && windowSampleCount > 0) {
      mqtt.loop();
      delay(100);
      if (publicarMQTT()) {
        bootstrapTelemetryPublished = true;
        lastMqttPublishMs = millis();
        Serial.println("[MQTT] Telemetria bootstrap publicada.");
      } else {
        Serial.println("[MQTT] Telemetria bootstrap omitida; se reintentara en loop().");
      }
    }
  }
}

void loop() {
  server.handleClient();

  if (strlen(MQTT_SERVER) > 0) {
    ensureWiFiConnected();

    unsigned long now = millis();

    if (WiFi.status() == WL_CONNECTED &&
        !mqtt.connected() &&
        (now - lastMqttAttemptMs >= MQTT_RECONNECT_INTERVAL_MS)) {
      lastMqttAttemptMs = now;
      reconnectMQTT();
    }

    if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
      mqtt.loop();
    }
  }

  unsigned long now = millis();

  if (now - lastControlSampleMs >= CONTROL_SAMPLE_MS) {
    lastControlSampleMs = now;
    lastRaw = readADC();
    lastSoilVwc = rawToSoilVwc(lastRaw);
  }

  if (now - lastAggregationSampleMs >= AGGREGATION_SAMPLE_MS) {
    lastAggregationSampleMs = now;
    captureAggregationSample();
  }

  if (now - lastMqttPublishMs >= MQTT_PUBLISH_INTERVAL_MS) {
    const bool published = publicarMQTT();
    lastMqttPublishMs = now;
    if (!published) {
      Serial.println("[MQTT] Ventana no publicada; se reintentara en el siguiente intervalo.");
    }
  }

  updateRelay(lastSoilVwc);
}
