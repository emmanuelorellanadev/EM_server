/*
  humedadSueloK8.ino
  ──────────────────────────────────────────────────────────────────
  Monitor de humedad de suelo para invernadero con:
    • Servidor web integrado  (GET /  y  GET /json)
    • Riego automático temporizado con período de enfriamiento
    • Publicación de datos vía MQTT para integración con EM_server

  Hardware objetivo:
    • ESP8266 NodeMCU V3  → seleccionar "NodeMCU 1.0 (ESP-12E Module)"
    • Sensor capacitivo/resistivo K8 / C11
        AO → A0   (lectura analógica 0-1023)
        DO → D5   (salida digital: LOW = húmedo, HIGH = seco)
    • Módulo relé 5 V active-HIGH conectado en D6
        relé abierto (LOW)  → electroválvula cerrada
        relé cerrado (HIGH) → electroválvula abierta
    • Electroválvula 12 V DC (contactos NO/COM del relé + diodo flyback)

  Cómo compilar y subir:
    1. Copia  config.example.h → config.h  y edita tus valores.
    2. En Arduino IDE:  Herramientas → Placa → "NodeMCU 1.0 (ESP-12E Module)"
    3. Instala la librería "PubSubClient" de Nick O'Leary (Library Manager).
    4. Compila y sube.

  Tópico MQTT publicado (MQTT_TOPICO en config.h):
    {
      "raw":      512,      ← lectura ADC cruda (0-1023)
      "percent":  42.5,     ← humedad de suelo en % (0 = seco, 100 = mojado)
      "state":    "MOIST",  ← "DRY" | "MOIST" | "WET"
      "watering": false,    ← ¿está el riego activo en este momento?
      "cooldown": false     ← ¿está en período de espera post-riego?
    }

  El servidor EM_server mapea automáticamente:
    "percent" → campo "soil_humidity"
    "raw"     → campo "soil_raw"
  ──────────────────────────────────────────────────────────────────
*/

#include "config.h"

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>

// ─────────────────────────────────────────────────────────────────
// Estado global
// ─────────────────────────────────────────────────────────────────
struct SensorState {
  int   raw       = 0;
  float percent   = 0.0f;
  bool  watering  = false;
  bool  cooldown  = false;
};

static SensorState g_state;

// Temporizadores
static unsigned long g_lastSample   = 0;   // última lectura/publicación
static unsigned long g_waterStart   = 0;   // inicio del ciclo de riego actual
static unsigned long g_coolStart    = 0;   // inicio del período de cooldown

// Objetos de red
WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);
ESP8266WebServer server(80);

// ─────────────────────────────────────────────────────────────────
// Utilidades
// ─────────────────────────────────────────────────────────────────

/** Convierte la lectura ADC cruda a porcentaje de humedad.
 *
 *  El sensor K8/C11 entrega un voltaje MAYOR cuando el suelo está SECO
 *  (mayor resistencia → mayor caída de tensión).  Por eso invertimos
 *  el mapa: ADC_SECO → 0 %, ADC_MOJADO → 100 %.
 */
float rawToPercent(int raw) {
  float pct = (float)(ADC_SECO - raw) / (float)(ADC_SECO - ADC_MOJADO) * 100.0f;
  if (pct < 0.0f)   pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  return pct;
}

/** Devuelve la etiqueta de estado en texto. */
const char* stateLabel(float pct) {
  if (pct < (float)UMBRAL_RIEGO)        return "DRY";
  if (pct < (float)UMBRAL_CORTE)        return "MOIST";
  return "WET";
}

/** Activa la electroválvula (relé HIGH). */
void startWatering() {
  if (g_state.cooldown || g_state.watering) return;
  digitalWrite(PIN_RELAY, HIGH);
  g_state.watering = true;
  g_waterStart = millis();
  Serial.println("[Riego] Iniciado.");
}

/** Detiene la electroválvula y arranca el cooldown. */
void stopWatering() {
  if (!g_state.watering) return;
  digitalWrite(PIN_RELAY, LOW);
  g_state.watering = false;
  g_state.cooldown = true;
  g_coolStart = millis();
  Serial.println("[Riego] Detenido. Cooldown iniciado.");
}

// ─────────────────────────────────────────────────────────────────
// Lectura del sensor
// ─────────────────────────────────────────────────────────────────
void readSensor() {
  g_state.raw     = analogRead(A0);
  g_state.percent = rawToPercent(g_state.raw);
}

// ─────────────────────────────────────────────────────────────────
// Lógica de riego automático
// ─────────────────────────────────────────────────────────────────
void updateWatering() {
  unsigned long now = millis();

  // Finalizar cooldown
  if (g_state.cooldown && (now - g_coolStart >= COOLDOWN_MS)) {
    g_state.cooldown = false;
    Serial.println("[Riego] Cooldown terminado.");
  }

  // Iniciar riego si el suelo está seco
  if (!g_state.watering && !g_state.cooldown
      && g_state.percent < (float)UMBRAL_RIEGO) {
    startWatering();
  }

  // Detener riego por tiempo máximo o suelo suficientemente húmedo
  if (g_state.watering) {
    bool timeout  = (now - g_waterStart >= DURACION_RIEGO_MS);
    bool soilWet  = (g_state.percent >= (float)UMBRAL_CORTE);
    if (timeout || soilWet) {
      stopWatering();
    }
  }
}

// ─────────────────────────────────────────────────────────────────
// Construcción del JSON de estado
// ─────────────────────────────────────────────────────────────────
String buildJson() {
  String j = "{";
  j += "\"raw\":"      + String(g_state.raw)                                + ",";
  j += "\"percent\":"  + String(g_state.percent, 1)                         + ",";
  j += "\"state\":\""  + String(stateLabel(g_state.percent))                + "\",";
  j += "\"watering\":" + String(g_state.watering ? "true" : "false")        + ",";
  j += "\"cooldown\":" + String(g_state.cooldown ? "true" : "false");
  j += "}";
  return j;
}

// ─────────────────────────────────────────────────────────────────
// Servidor web
// ─────────────────────────────────────────────────────────────────
void handleRoot() {
  String estado     = stateLabel(g_state.percent);
  String riegoBadge = g_state.watering ? "💧 Activo" : "⏸ Inactivo";
  String coolBadge  = g_state.cooldown ? "⏳ En espera" : "✅ Listo";

  String html = "<!DOCTYPE html><html lang='es'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='30'>"
    "<title>Invernadero – Humedad Suelo</title>"
    "<style>"
    "body{font-family:sans-serif;background:#f4f6f4;margin:0;padding:1rem}"
    "h1{color:#2e7d32}h2{color:#555;font-size:1rem}"
    ".card{background:#fff;border-radius:12px;padding:1rem 1.2rem;"
           "margin:.6rem 0;box-shadow:0 2px 8px rgba(0,0,0,.1)}"
    ".val{font-size:2.5rem;font-weight:700;color:#1b5e20}"
    ".badge{display:inline-block;padding:.2rem .6rem;border-radius:6px;"
            "font-size:.85rem;font-weight:600;margin-top:.3rem}"
    ".wet{background:#e3f2fd;color:#0d47a1}"
    ".moist{background:#e8f5e9;color:#1b5e20}"
    ".dry{background:#fff3e0;color:#e65100}"
    ".on{background:#ffebee;color:#b71c1c}"
    ".off{background:#e8f5e9;color:#2e7d32}"
    "a{color:#2e7d32}"
    "</style></head><body>"
    "<h1>🌱 Invernadero – Monitor de Humedad</h1>"
    "<h2>IP: " + WiFi.localIP().toString() + "</h2>"
    "<div class='card'>"
      "<div class='val'>" + String(g_state.percent, 1) + " %</div>"
      "<div>Humedad del suelo</div>"
      "<span class='badge " + (estado == "DRY" ? "dry" : (estado == "WET" ? "wet" : "moist")) + "'>"
        + estado + "</span>"
    "</div>"
    "<div class='card'>ADC Raw: <b>" + String(g_state.raw) + "</b> / 1023</div>"
    "<div class='card'>Riego: <span class='badge " + (g_state.watering ? "on" : "off") + "'>"
      + riegoBadge + "</span></div>"
    "<div class='card'>Cooldown: <span class='badge " + (g_state.cooldown ? "on" : "off") + "'>"
      + coolBadge + "</span></div>"
    "<p><a href='/json'>Ver JSON</a></p>"
    "</body></html>";

  server.send(200, "text/html; charset=UTF-8", html);
}

void handleJson() {
  server.send(200, "application/json", buildJson());
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ─────────────────────────────────────────────────────────────────
// MQTT
// ─────────────────────────────────────────────────────────────────
bool mqttEnabled() {
  return strlen(MQTT_SERVER) > 0;
}

void mqttReconnect() {
  if (!mqttEnabled() || mqttClient.connected()) return;
  Serial.print("[MQTT] Conectando a ");
  Serial.print(MQTT_SERVER);
  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD);
  } else {
    ok = mqttClient.connect(MQTT_CLIENT_ID);
  }
  if (ok) {
    Serial.println(" → OK");
  } else {
    Serial.print(" → FALLO rc=");
    Serial.println(mqttClient.state());
  }
}

void mqttPublish() {
  if (!mqttEnabled()) return;
  mqttReconnect();
  if (!mqttClient.connected()) return;
  String payload = buildJson();
  mqttClient.publish(MQTT_TOPICO, payload.c_str(), /*retained=*/false);
  Serial.print("[MQTT] Publicado en ");
  Serial.print(MQTT_TOPICO);
  Serial.print(": ");
  Serial.println(payload);
}

// ─────────────────────────────────────────────────────────────────
// setup() y loop()
// ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[EM_server] Invernadero – Monitor de Humedad Suelo");

  // Configurar pines
  pinMode(PIN_SENSOR_DO, INPUT);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);  // relé apagado por defecto

  // Conectar Wi-Fi
  Serial.print("[WiFi] Conectando a ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[WiFi] Conectado. IP: ");
  Serial.println(WiFi.localIP());

  // Configurar MQTT
  if (mqttEnabled()) {
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setBufferSize(256);
    mqttReconnect();
  }

  // Configurar servidor web
  server.on("/",     handleRoot);
  server.on("/json", handleJson);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[Web] Servidor iniciado en puerto 80");

  // Primera lectura
  readSensor();
  updateWatering();
  g_lastSample = millis();
}

void loop() {
  // Atender peticiones web
  server.handleClient();

  // Mantener conexión MQTT
  if (mqttEnabled()) {
    if (!mqttClient.connected()) mqttReconnect();
    mqttClient.loop();
  }

  // Muestreo periódico en segundo plano
  unsigned long now = millis();
  if (now - g_lastSample >= BACKGROUND_SAMPLE_MS) {
    g_lastSample = now;
    readSensor();
    updateWatering();
    mqttPublish();
    Serial.print("[Sensor] raw=");
    Serial.print(g_state.raw);
    Serial.print("  pct=");
    Serial.print(g_state.percent, 1);
    Serial.print("%  state=");
    Serial.print(stateLabel(g_state.percent));
    Serial.print("  riego=");
    Serial.print(g_state.watering ? "SI" : "NO");
    Serial.print("  cooldown=");
    Serial.println(g_state.cooldown ? "SI" : "NO");
  }

  // Actualizar lógica de riego continuamente (sin bloquear)
  updateWatering();
}
