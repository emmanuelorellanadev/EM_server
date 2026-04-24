/*
  humedadSueloK8.ino
  ══════════════════════════════════════════════════════════════════════════════
  Monitor de humedad de suelo para invernadero.

  FUNCIONALIDADES
  ───────────────
  • Lee el sensor K8/C11 por el ADC y convierte la lectura a % de humedad.
  • Controla una electroválvula a través de un módulo relé para riego automático.
  • Expone un servidor web HTTP en el puerto 80 para consultar el estado local.
  • PUBLICA datos de sensor vía MQTT hacia la Raspberry Pi (EM_server).
  • SUSCRIBE a un tópico MQTT de comandos: la Raspberry Pi puede ordenar un
    ciclo de riego manual enviando {"action":"water"} al tópico definido
    en MQTT_TOPICO_CMD (config.h).

  HARDWARE
  ────────
  • ESP8266 NodeMCU V3   (CPU Tensilica L106 @ 80 MHz, 80 kB RAM, 4 MB Flash)
  • Sensor K8/C11         (resistivo: AO → A0 analógico, DO → D5 digital)
  • Módulo relé 5 V       (active-HIGH en D6/GPIO12)
  • Electroválvula 12 V DC (NC/COM del relé + diodo flyback 1N4007)

  CÓMO COMPILAR Y SUBIR
  ──────────────────────
  1. Copia config.example.h → config.h y edita tus valores.
  2. En Arduino IDE: Herramientas → Placa → "NodeMCU 1.0 (ESP-12E Module)"
  3. Instala "PubSubClient" de Nick O'Leary (Library Manager).
  4. Compila y sube.

  PROTOCOLO MQTT — FLUJO DE DATOS
  ────────────────────────────────
  Publicación (ESP8266 → Raspberry Pi):
    Tópico : MQTT_TOPICO   (por defecto "sensors/esp8266")
    Payload: {"raw":512,"percent":42.3,"state":"MOIST","watering":false,"cooldown":false}
    Cadencia: cada BACKGROUND_SAMPLE_MS milisegundos

  Suscripción (Raspberry Pi → ESP8266):
    Tópico : MQTT_TOPICO_CMD  (por defecto "commands/esp8266")
    Payload: {"action":"water"}
    Efecto : activa un ciclo de riego manual por DURACION_RIEGO_MS ms.
             A diferencia del riego automático, el manual cancela el cooldown
             activo si lo hubiera, permitiendo al usuario forzar el riego.

  MAPEO EN EL SERVIDOR EM_server
  ────────────────────────────────
    "percent" → campo "soil_humidity"   (configurado en config.json/field_mappings)
    "raw"     → campo "soil_raw"
  ══════════════════════════════════════════════════════════════════════════════
*/

// ─────────────────────────────────────────────────────────────────────────────
// CABECERAS (HEADERS) — Inclusión de librerías
// ─────────────────────────────────────────────────────────────────────────────

// config.h contiene las constantes de configuración (Wi-Fi, MQTT, pines…).
// Al usar comillas en lugar de <>, el compilador busca el archivo en el mismo
// directorio que el sketch en vez de en los directorios del sistema.
#include "config.h"

// ESP8266WiFi.h — librería Wi-Fi del SDK del ESP8266.
//   Proporciona la clase WiFi (objeto global estático) con métodos para:
//     WiFi.begin(ssid, pass)      → iniciar conexión
//     WiFi.status()               → comprobar estado (WL_CONNECTED, etc.)
//     WiFi.localIP()              → obtener IP asignada por DHCP
//   La comunicación real la realiza el coprocesador Wi-Fi del ESP8266 (ESP-07S)
//   en segundo plano mientras el código principal ejecuta el loop().
#include <ESP8266WiFi.h>

// ESP8266WebServer.h — servidor HTTP integrado (single-threaded, no-blocking).
//   Permite registrar "handlers" (funciones callback) para rutas HTTP:
//     server.on("/ruta", handlerFn)  → asocia URL con función
//     server.handleClient()          → procesa peticiones pendientes (llamar en loop)
//   El servidor acepta una conexión a la vez; es suficiente para un invernadero.
#include <ESP8266WebServer.h>

// PubSubClient.h — cliente MQTT para Arduino/ESP8266.
//   Autor: Nick O'Leary (Knolleary). Implementa MQTT v3.1.1 sobre TCP.
//   Patrón de uso:
//     mqttClient.setServer(host, port)     → configura el broker
//     mqttClient.setCallback(fn)           → función llamada al recibir un mensaje
//     mqttClient.connect(clientId)         → conecta al broker
//     mqttClient.subscribe(topic)          → suscribe a un tópico
//     mqttClient.publish(topic, payload)   → publica un mensaje
//     mqttClient.loop()                    → procesa mensajes entrantes (llamar en loop)
//   Limitación: el buffer de mensajes es configurable (setBufferSize); por defecto 128 B.
#include <PubSubClient.h>

// ─────────────────────────────────────────────────────────────────────────────
// ESTADO GLOBAL
// ─────────────────────────────────────────────────────────────────────────────

/*
 * SensorState agrupa todas las variables de estado del sistema en una sola
 * estructura. Usar una struct en lugar de variables sueltas facilita pasar
 * el estado a funciones y deja claro qué pertenece al "estado del sistema".
 */
struct SensorState {
  int   raw       = 0;      // lectura ADC cruda (0 = mojado, 1023 = seco)
  float percent   = 0.0f;   // humedad convertida a porcentaje (0% = seco, 100% = mojado)
  bool  watering  = false;  // true mientras la electroválvula está abierta
  bool  cooldown  = false;  // true durante la espera obligatoria post-riego
};

// g_state es la única fuente de verdad del estado del sistema.
// El prefijo "g_" marca que es una variable global (estándar del proyecto).
static SensorState g_state;

/*
 * Temporizadores — usamos unsigned long (32 bits) porque millis() devuelve
 * ese tipo. El contador de millis() se desborda a cero cada ~49 días, pero
 * la aritmética de desbordamiento sin signo es correcta: si g_lastSample=0
 * y millis() vale 4294967290 tras el desbordamiento, la diferencia da el
 * tiempo real transcurrido sin necesidad de ningún ajuste especial.
 */
static unsigned long g_lastSample = 0;  // marca de tiempo del último muestreo
static unsigned long g_waterStart = 0;  // instante en que comenzó el riego actual
static unsigned long g_coolStart  = 0;  // instante en que comenzó el cooldown actual

// ─────────────────────────────────────────────────────────────────────────────
// OBJETOS DE RED
// ─────────────────────────────────────────────────────────────────────────────

/*
 * WiFiClient implementa la interfaz Client de Arduino (read/write/connect).
 * PubSubClient lo usa internamente para enviar y recibir bytes TCP.
 * Al pasarlo al constructor de PubSubClient le indicamos que use TCP simple
 * (sin TLS). Para TLS se usaría WiFiClientSecure.
 */
WiFiClient   wifiClient;

/*
 * mqttClient es el objeto central de MQTT. Internamente mantiene:
 *   • El socket TCP (vía wifiClient)
 *   • El buffer de entrada/salida de mensajes MQTT
 *   • El estado de conexión y el keep-alive (PINGREQ/PINGRESP)
 */
PubSubClient mqttClient(wifiClient);

/*
 * server escucha en el puerto 80 (HTTP estándar). No necesita credenciales
 * ya que es un servidor local de invernadero en red privada.
 */
ESP8266WebServer server(80);

// ─────────────────────────────────────────────────────────────────────────────
// UTILIDADES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * rawToPercent — Convierte la lectura ADC cruda a porcentaje de humedad.
 *
 * El ADC del ESP8266 convierte una tensión analógica (0-3.3 V en el pin A0,
 * aunque el NodeMCU tiene un divisor resistivo que escala a 0-1 V externo)
 * a un valor digital de 10 bits: 0-1023.
 *
 * El sensor K8/C11 es RESISTIVO: su resistencia interna cae cuando el suelo
 * está húmedo, lo que provoca una tensión de salida MENOR. Por eso el mapa
 * está INVERTIDO respecto a lo intuitivo:
 *   ADC alto (≈ ADC_SECO=1023) → suelo SECO   → 0%
 *   ADC bajo (≈ ADC_MOJADO=300) → suelo MOJADO → 100%
 *
 * La fórmula de interpolación lineal y el clamping final (constrain a [0,100])
 * protegen contra lecturas fuera del rango de calibración.
 *
 * @param raw  Valor ADC leído de analogRead(A0), rango 0-1023.
 * @return     Porcentaje de humedad en [0.0, 100.0].
 */
float rawToPercent(int raw) {
  // Interpolación lineal inversa: cuanto mayor el ADC, menor la humedad.
  float pct = (float)(ADC_SECO - raw) / (float)(ADC_SECO - ADC_MOJADO) * 100.0f;
  // Clamping: fuerza el resultado al rango [0, 100] para no reportar valores
  // absurdos si el sensor está fuera del suelo o mal calibrado.
  if (pct < 0.0f)   pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  return pct;
}

/**
 * stateLabel — Devuelve una etiqueta textual según el nivel de humedad.
 *
 * Tres estados cubren los casos de interés para el riego:
 *   DRY   → suelo seco, por debajo del umbral de riego
 *   MOIST → humedad aceptable, entre los dos umbrales
 *   WET   → suelo húmedo, por encima del umbral de corte
 *
 * @param pct  Porcentaje de humedad [0, 100].
 * @return     Cadena literal (almacenada en Flash, no en RAM).
 */
const char* stateLabel(float pct) {
  if (pct < (float)UMBRAL_RIEGO) return "DRY";
  if (pct < (float)UMBRAL_CORTE) return "MOIST";
  return "WET";
}

// ─────────────────────────────────────────────────────────────────────────────
// CONTROL DEL RIEGO
// ─────────────────────────────────────────────────────────────────────────────

/**
 * startWatering — Activa la electroválvula (relé HIGH).
 *
 * @param force  Si es true, cancela el cooldown activo y fuerza el inicio
 *               del riego aunque el sistema esté en período de espera.
 *               Usado por comandos manuales desde la Raspberry Pi.
 *               Si es false (valor por defecto), respeta el cooldown.
 *
 * El relé es de tipo "active-HIGH":
 *   GPIO HIGH → bobina del relé energizada → contacto NO cierra → válvula abre.
 *   GPIO LOW  → bobina desactivada         → contacto NO abre   → válvula cierra.
 *
 * El diodo flyback (1N4007) en paralelo con la bobina absorbe el pico de
 * tensión inversa ("kick-back") que aparece al desactivar la bobina del relé,
 * protegiendo el pin GPIO del ESP8266 de voltajes destructivos.
 */
void startWatering(bool force = false) {
  // Si ya estamos regando, no hacer nada (evitar reset del temporizador).
  if (g_state.watering) return;

  // Si hay cooldown activo y no es un comando forzado, respetar la espera.
  if (g_state.cooldown && !force) return;

  // Cancelar cooldown si el comando es forzado (iniciado manualmente).
  if (force && g_state.cooldown) {
    g_state.cooldown = false;
    Serial.println("[Riego] Cooldown cancelado por comando manual.");
  }

  // Activar el relé: HIGH pone tensión en la bobina → contacto NO cierra.
  digitalWrite(PIN_RELAY, HIGH);
  g_state.watering = true;
  g_waterStart = millis();  // guarda el instante de inicio para calcular la duración

  Serial.println("[Riego] Iniciado.");
}

/**
 * stopWatering — Desactiva la electroválvula e inicia el período de cooldown.
 *
 * El cooldown previene que el suelo se embarre al regar en ciclos demasiado
 * cortos. El sistema espera COOLDOWN_MS antes de permitir otro ciclo automático
 * (el riego manual desde la Raspberry Pi sí puede saltarse el cooldown).
 */
void stopWatering() {
  // Protección: no hacer nada si ya está detenido.
  if (!g_state.watering) return;

  // Desactivar el relé: LOW desactiva la bobina → contacto NO abre → válvula cierra.
  digitalWrite(PIN_RELAY, LOW);
  g_state.watering = false;

  // Iniciar el período de cooldown.
  g_state.cooldown = true;
  g_coolStart = millis();

  Serial.println("[Riego] Detenido. Cooldown iniciado.");
}

// ─────────────────────────────────────────────────────────────────────────────
// LECTURA DEL SENSOR
// ─────────────────────────────────────────────────────────────────────────────

/**
 * readSensor — Toma una muestra del sensor K8/C11.
 *
 * analogRead(A0) devuelve un entero de 10 bits (0-1023) proporcional a la
 * tensión en el pin A0. La conversión tarda ~100 µs en el ESP8266.
 * El resultado se almacena en g_state para que todas las funciones del
 * sistema tengan acceso a la última lectura sin re-muestrear el sensor.
 */
void readSensor() {
  g_state.raw     = analogRead(A0);          // lectura ADC cruda
  g_state.percent = rawToPercent(g_state.raw); // conversión a %
}

// ─────────────────────────────────────────────────────────────────────────────
// LÓGICA DE RIEGO AUTOMÁTICO
// ─────────────────────────────────────────────────────────────────────────────

/**
 * updateWatering — Máquina de estados del riego automático.
 *
 * Esta función se llama frecuentemente desde loop() y evalúa las transiciones
 * entre los tres estados del sistema de riego:
 *
 *   IDLE ──(humedad < UMBRAL_RIEGO)──► WATERING
 *   WATERING ──(tiempo ≥ DURACION_RIEGO_MS ó humedad ≥ UMBRAL_CORTE)──► COOLDOWN
 *   COOLDOWN ──(tiempo ≥ COOLDOWN_MS)──► IDLE
 *
 * El uso de millis() en lugar de delay() es fundamental en Arduino/ESP8266:
 * delay() bloquea el procesador e impide atender el servidor web, el cliente
 * MQTT y el watchdog (WDT). millis() devuelve el tiempo transcurrido desde
 * el arranque sin bloquear el loop principal (programación no-bloqueante).
 */
void updateWatering() {
  unsigned long now = millis();

  // ── Transición COOLDOWN → IDLE ─────────────────────────────────
  if (g_state.cooldown && (now - g_coolStart >= COOLDOWN_MS)) {
    g_state.cooldown = false;
    Serial.println("[Riego] Cooldown terminado. Sistema listo.");
  }

  // ── Transición IDLE → WATERING (riego automático) ──────────────
  // Solo si: no estamos regando, no hay cooldown, Y el suelo está seco.
  if (!g_state.watering && !g_state.cooldown
      && g_state.percent < (float)UMBRAL_RIEGO) {
    startWatering(/*force=*/false);
  }

  // ── Transición WATERING → COOLDOWN ─────────────────────────────
  if (g_state.watering) {
    bool timeout = (now - g_waterStart >= DURACION_RIEGO_MS); // riego demasiado largo
    bool soilWet = (g_state.percent >= (float)UMBRAL_CORTE);  // suelo ya húmedo
    if (timeout || soilWet) {
      stopWatering();
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// CONSTRUCCIÓN DEL PAYLOAD JSON
// ─────────────────────────────────────────────────────────────────────────────

/**
 * buildJson — Construye el string JSON con el estado actual.
 *
 * El payload es el mismo para la publicación MQTT y para el endpoint GET /json.
 * Construir el JSON manualmente (sin librería) es suficiente aquí porque el
 * formato es fijo y pequeño. Para payloads dinámicos o anidados se recomienda
 * la librería ArduinoJson.
 *
 * Ejemplo de salida:
 *   {"raw":512,"percent":42.3,"state":"MOIST","watering":false,"cooldown":false}
 *
 * El servidor EM_server mapea "percent" → "soil_humidity" y "raw" → "soil_raw"
 * mediante la tabla field_mappings de config.json.
 *
 * @return  String de Arduino con el JSON completo.
 */
String buildJson() {
  String j = "{";
  j += "\"raw\":"      + String(g_state.raw)                         + ",";
  j += "\"percent\":"  + String(g_state.percent, 1)                  + ",";
  j += "\"state\":\""  + String(stateLabel(g_state.percent))         + "\",";
  j += "\"watering\":" + String(g_state.watering ? "true" : "false") + ",";
  j += "\"cooldown\":" + String(g_state.cooldown ? "true" : "false");
  j += "}";
  return j;
}

// ─────────────────────────────────────────────────────────────────────────────
// SERVIDOR WEB HTTP
// ─────────────────────────────────────────────────────────────────────────────

/**
 * handleRoot — Handler de GET /
 *
 * Genera una página HTML con el estado actual del sensor y el riego.
 * La página se auto-refresca cada 30 segundos mediante la cabecera
 * <meta http-equiv='refresh'>.
 *
 * El HTML se construye como String de Arduino porque no hay sistema de
 * archivos disponible en este sketch (se podría usar LittleFS para servir
 * archivos estáticos, pero aumenta la complejidad del proyecto).
 */
void handleRoot() {
  String estado     = stateLabel(g_state.percent);
  String riegoBadge = g_state.watering ? "💧 Activo"    : "⏸ Inactivo";
  String coolBadge  = g_state.cooldown ? "⏳ En espera" : "✅ Listo";

  String html =
    "<!DOCTYPE html><html lang='es'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='30'>"  // auto-recarga cada 30 s
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
    ".on{background:#fff3e0;color:#e65100}"   // ámbar = atención (riego activo)
    ".off{background:#e8f5e9;color:#2e7d32}"  // verde  = inactivo/listo
    "a{color:#2e7d32}"
    "</style></head><body>"
    "<h1>🌱 Invernadero – Monitor de Humedad</h1>"
    "<h2>IP: " + WiFi.localIP().toString() + "</h2>"
    // Tarjeta principal: valor de humedad + etiqueta de estado
    "<div class='card'>"
      "<div class='val'>" + String(g_state.percent, 1) + " %</div>"
      "<div>Humedad del suelo</div>"
      "<span class='badge " +
        (estado == "DRY" ? "dry" : (estado == "WET" ? "wet" : "moist")) +
      "'>" + estado + "</span>"
    "</div>"
    "<div class='card'>ADC Raw: <b>" + String(g_state.raw) + "</b> / 1023</div>"
    "<div class='card'>Riego: <span class='badge " +
      (g_state.watering ? "on" : "off") + "'>" + riegoBadge + "</span></div>"
    "<div class='card'>Cooldown: <span class='badge " +
      (g_state.cooldown ? "on" : "off") + "'>" + coolBadge + "</span></div>"
    // Enlace a la API JSON para integraciones externas
    "<p><a href='/json'>Ver JSON</a></p>"
    "</body></html>";

  // Tipo MIME explícito con charset para que el navegador muestre tildes.
  server.send(200, "text/html; charset=UTF-8", html);
}

/**
 * handleJson — Handler de GET /json
 *
 * Devuelve el estado actual como JSON. Útil para:
 *   • Depuración rápida con curl o el navegador.
 *   • Integración con sistemas externos que no usan MQTT.
 */
void handleJson() {
  server.send(200, "application/json", buildJson());
}

/**
 * handleNotFound — Handler de rutas no registradas (404).
 */
void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT — PUBLICACIÓN Y SUSCRIPCIÓN
// ─────────────────────────────────────────────────────────────────────────────

/**
 * mqttEnabled — Comprueba si MQTT está habilitado en config.h.
 *
 * Si MQTT_SERVER es una cadena vacía (""), toda la lógica MQTT se omite.
 * Esto permite usar el sketch sin broker MQTT, solo con el servidor web.
 */
bool mqttEnabled() {
  return strlen(MQTT_SERVER) > 0;
}

/**
 * mqttCallback — Función llamada por PubSubClient al recibir un mensaje MQTT.
 *
 * CONCEPTO: En el patrón publicar/suscribir, el broker entrega mensajes de
 * forma asíncrona. PubSubClient no usa threads; en cambio, cuando se llama a
 * mqttClient.loop() procesa los mensajes en la cola y llama a esta función
 * para cada mensaje recibido. Es un patrón "event-driven" dentro de un loop
 * single-threaded.
 *
 * FIRMA FIJA: PubSubClient exige exactamente estos tres parámetros:
 *   topic    → nombre del tópico del mensaje recibido (cadena terminada en '\0')
 *   payload  → puntero al array de bytes del cuerpo del mensaje
 *   length   → longitud del payload en bytes (sin terminador nulo)
 *
 * COMANDO SOPORTADO:
 *   Tópico : MQTT_TOPICO_CMD  (p.ej. "commands/esp8266")
 *   Payload: {"action":"water"}
 *   Efecto : activa un ciclo de riego por DURACION_RIEGO_MS.
 *            El riego manual cancela el cooldown si estuviera activo.
 *
 * @param topic    Tópico del mensaje recibido.
 * @param payload  Bytes del cuerpo del mensaje (NO es un String terminado en '\0').
 * @param length   Número de bytes en payload.
 */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Convertir payload (bytes) a String de Arduino para facilitar el parsing.
  // Creamos el String con longitud explícita para evitar que se corte si el
  // payload contiene bytes '\0' internos (poco probable en JSON, pero buena práctica).
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.print("[MQTT] Mensaje en tópico '");
  Serial.print(topic);
  Serial.print("': ");
  Serial.println(msg);

  // Procesar comandos recibidos en el tópico de comandos.
  // Comparamos el tópico completo para no confundir con otros posibles tópicos
  // a los que pudiera suscribirse el cliente en el futuro.
  if (String(topic) == String(MQTT_TOPICO_CMD)) {
    // Búsqueda simple del campo "action":"water" sin parsear JSON completo.
    // Esto evita agregar la librería ArduinoJson solo para un caso de uso simple.
    // Si en el futuro se añaden más comandos, se recomienda usar ArduinoJson.
    if (msg.indexOf("\"action\"") >= 0 && msg.indexOf("\"water\"") >= 0) {
      Serial.println("[Riego] Comando manual recibido desde Raspberry Pi.");
      // force=true: cancela cooldown activo y arranca el riego inmediatamente.
      startWatering(/*force=*/true);
    } else {
      Serial.print("[MQTT] Comando desconocido: ");
      Serial.println(msg);
    }
  }
}

/**
 * mqttReconnect — Conecta (o reconecta) al broker MQTT.
 *
 * Se llama en setup() y también en loop() cuando se detecta desconexión.
 * Tras una reconexión exitosa restablece TODAS las suscripciones, ya que
 * el broker descarta las suscripciones al desconectarse (para clientes sin
 * sesión persistente, que es el caso con cleanSession=true por defecto).
 *
 * KEEP-ALIVE: PubSubClient envía automáticamente mensajes PINGREQ al broker
 * cada keepalive/2 segundos para mantener la sesión TCP activa. El broker
 * desconecta al cliente si no recibe ningún mensaje en keepalive segundos.
 */
void mqttReconnect() {
  // No hacer nada si MQTT no está configurado o si ya estamos conectados.
  if (!mqttEnabled() || mqttClient.connected()) return;

  Serial.print("[MQTT] Conectando a ");
  Serial.print(MQTT_SERVER);
  Serial.print("...");

  bool ok;
  // La autenticación es opcional: si MQTT_USER está vacío, conectamos sin credenciales.
  if (strlen(MQTT_USER) > 0) {
    ok = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD);
  } else {
    ok = mqttClient.connect(MQTT_CLIENT_ID);
  }

  if (ok) {
    Serial.println(" OK");

    // Suscribirse al tópico de comandos para recibir instrucciones remotas.
    // QoS 1 (segundo parámetro = 1): el broker garantiza entregar el mensaje
    // al menos una vez. Si se usa QoS 0, el mensaje puede perderse en redes
    // con pérdida de paquetes.
    mqttClient.subscribe(MQTT_TOPICO_CMD, /*qos=*/1);
    Serial.print("[MQTT] Suscrito a: ");
    Serial.println(MQTT_TOPICO_CMD);
  } else {
    // mqttClient.state() devuelve un código de error numérico:
    //  -4 MQTT_CONNECTION_TIMEOUT      → broker no responde
    //  -3 MQTT_CONNECTION_LOST         → conexión TCP interrumpida
    //  -2 MQTT_CONNECT_FAILED          → error de red
    //  -1 MQTT_DISCONNECTED            → no intentó conectar
    //   1 MQTT_CONNECT_BAD_PROTOCOL    → versión de protocolo rechazada
    //   2 MQTT_CONNECT_BAD_CLIENT_ID   → client_id rechazado por el broker
    //   5 MQTT_CONNECT_UNAUTHORIZED    → credenciales inválidas
    Serial.print(" FALLO rc=");
    Serial.println(mqttClient.state());
  }
}

/**
 * mqttPublish — Publica el estado actual del sensor en el broker MQTT.
 *
 * El payload JSON es leído por el servicio mqtt_client.py en la Raspberry Pi,
 * que lo persiste en la base de datos SQLite y lo muestra en el dashboard web.
 *
 * retained=false: el broker no guarda el mensaje para nuevos suscriptores.
 * Si se pusiera true, cualquier cliente que se conecte recibiría la última
 * lectura inmediatamente, lo cual puede ser útil pero también puede mostrar
 * datos desactualizados.
 */
void mqttPublish() {
  if (!mqttEnabled()) return;
  mqttReconnect();
  if (!mqttClient.connected()) return;

  String payload = buildJson();
  mqttClient.publish(MQTT_TOPICO, payload.c_str(), /*retained=*/false);

  Serial.print("[MQTT] Publicado en '");
  Serial.print(MQTT_TOPICO);
  Serial.print("': ");
  Serial.println(payload);
}

// ─────────────────────────────────────────────────────────────────────────────
// SETUP — Inicialización (se ejecuta una sola vez al arrancar)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * setup() — Punto de entrada de Arduino para la inicialización.
 *
 * Arduino (y por extensión el SDK del ESP8266) ejecuta setup() una sola vez
 * al encender o reiniciar el microcontrolador. Aquí se configura el hardware
 * y se establecen las conexiones de red antes de entrar al loop principal.
 */
void setup() {
  // Iniciar la comunicación serie a 115200 baudios para depuración.
  // El Monitor Serie de Arduino IDE debe configurarse a la misma velocidad.
  // 115200 bps es el estándar para el ESP8266 (velocidades mayores pueden
  // causar errores por la variación de la frecuencia del oscilador interno).
  Serial.begin(115200);
  delay(100);  // espera breve para que el puerto serie se estabilice
  Serial.println("\n[EM_server] Invernadero – Monitor de Humedad Suelo");
  Serial.println("Versión con riego remoto vía MQTT.");

  // ── Configurar pines de hardware ───────────────────────────────
  // INPUT: el pin solo lee tensión, no entrega corriente.
  // OUTPUT: el pin puede entregar hasta 12 mA en GPIO del ESP8266.
  // El relé se inicializa LOW (apagado) para evitar abrir la válvula
  // accidentalmente durante el arranque del sistema.
  pinMode(PIN_SENSOR_DO, INPUT);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);  // relé apagado al arrancar
  Serial.println("[HW] Pines configurados. Relé apagado.");

  // ── Conectar a la red Wi-Fi ────────────────────────────────────
  // WiFi.begin() inicia el proceso de asociación en segundo plano.
  // El bucle while espera a que el coprocesador Wi-Fi complete la
  // autenticación WPA2 y reciba una IP por DHCP (puede tardar 1-5 s).
  Serial.print("[WiFi] Conectando a ");
  Serial.print(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[WiFi] Conectado. Dirección IP: ");
  Serial.println(WiFi.localIP());

  // ── Configurar cliente MQTT ────────────────────────────────────
  if (mqttEnabled()) {
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);

    // Registrar la función callback que se llama al recibir mensajes.
    // DEBE hacerse ANTES de llamar a mqttReconnect()/connect().
    mqttClient.setCallback(mqttCallback);

    // Ampliar el buffer interno de PubSubClient a 256 bytes.
    // El buffer por defecto (128 bytes) puede ser insuficiente para
    // payloads JSON grandes. Si un mensaje excede el buffer, se descarta.
    mqttClient.setBufferSize(256);

    mqttReconnect();  // primer intento de conexión al broker
  } else {
    Serial.println("[MQTT] Deshabilitado (MQTT_SERVER vacío en config.h).");
  }

  // ── Configurar servidor web ────────────────────────────────────
  // server.on() registra pares (ruta HTTP, función handler).
  // El servidor maneja una petición a la vez de forma síncrona.
  server.on("/",     handleRoot);   // página HTML de estado
  server.on("/json", handleJson);   // API JSON para integraciones
  server.onNotFound(handleNotFound); // handler genérico para rutas desconocidas
  server.begin();
  Serial.println("[Web] Servidor HTTP iniciado en puerto 80.");
  Serial.print("[Web] Abre en el navegador: http://");
  Serial.println(WiFi.localIP());

  // ── Primera lectura y publicación ─────────────────────────────
  readSensor();
  updateWatering();
  mqttPublish();
  g_lastSample = millis();

  Serial.println("[OK] Sistema inicializado y listo.");
}

// ─────────────────────────────────────────────────────────────────────────────
// LOOP — Bucle principal (se ejecuta continuamente)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * loop() — Bucle principal de Arduino.
 *
 * Se ejecuta repetidamente y sin retardo fijo. La filosofía es "no-blocking":
 * ninguna tarea debe detener el procesador por más de unos pocos milisegundos
 * para que las demás tareas sigan respondiendo.
 *
 * Tareas en cada iteración:
 *  1. Atender peticiones HTTP pendientes.
 *  2. Mantener la conexión MQTT y procesar mensajes entrantes (comandos).
 *  3. Evaluar continuamente la lógica de riego (timeouts, fin de cooldown).
 *  4. Cada BACKGROUND_SAMPLE_MS: leer sensor, publicar por MQTT y loguear.
 */
void loop() {
  // ── 1. Servidor web ────────────────────────────────────────────
  // handleClient() procesa hasta una petición HTTP por llamada.
  // Llamarlo en cada iteración del loop garantiza tiempos de respuesta bajos.
  server.handleClient();

  // ── 2. Cliente MQTT ────────────────────────────────────────────
  if (mqttEnabled()) {
    // Reconectar si la conexión TCP se cayó (p.ej. reinicio del broker).
    if (!mqttClient.connected()) mqttReconnect();

    // mqttClient.loop() es IMPRESCINDIBLE:
    //   • Lee bytes del socket TCP y ensambla mensajes MQTT entrantes.
    //   • Llama a mqttCallback() para cada mensaje en el tópico suscrito.
    //   • Envía PINGREQ al broker para mantener el keep-alive.
    //   Sin esta llamada, los mensajes entrantes (comandos de riego) nunca
    //   se procesarían y el broker desconectaría al cliente por timeout.
    mqttClient.loop();
  }

  // ── 3. Lógica de riego (evaluación continua) ───────────────────
  // Se llama en cada iteración para detectar rápidamente el fin del tiempo
  // de riego (DURACION_RIEGO_MS) y el fin del cooldown (COOLDOWN_MS),
  // sin depender del intervalo de muestreo periódico.
  updateWatering();

  // ── 4. Muestreo periódico y publicación MQTT ───────────────────
  // La comparación (now - g_lastSample >= BACKGROUND_SAMPLE_MS) es
  // safe ante desbordamiento de millis() gracias a la aritmética sin signo.
  unsigned long now = millis();
  if (now - g_lastSample >= BACKGROUND_SAMPLE_MS) {
    g_lastSample = now;

    readSensor();     // actualiza g_state.raw y g_state.percent
    updateWatering(); // re-evalúa riego con la nueva lectura
    mqttPublish();    // envía datos al broker MQTT

    // Log por Serial para depuración con el Monitor Serie de Arduino IDE.
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
}
