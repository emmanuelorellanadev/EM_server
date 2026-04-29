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
    • ESP8266 NodeMCU V3
        Seleccionar en Arduino IDE: "NodeMCU 1.0 (ESP-12E Module)"
        Microcontrolador con Wi-Fi integrado. Tiene un único pin ADC (A0)
        que lee voltajes entre 0 V y 1 V (internamente con divisor de voltaje
        se puede leer hasta 3.3 V según la placa).

    • Sensor capacitivo/resistivo K8 o C11
        AO (Analog Output) → A0  : señal analógica de 0–1023 según humedad
        DO (Digital Output) → D5 : salida digital (umbral fijo, no usado en control)
        VCC → 3.3 V o 5 V según modelo
        GND → GND

    • Módulo relé 5 V
        IN  → D1 (GPIO 5)  : señal de control desde el ESP8266
        VCC → 5 V externo  : alimentación de la bobina del relé
        GND → GND compartido con el ESP8266
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
    4. Selecciona placa "NodeMCU 1.0 (ESP-12E Module)" y compila/sube.

  ENDPOINTS DEL SERVIDOR WEB LOCAL:
  ────────────────────────────────────
    GET /       → Página HTML amigable con estado actual del sistema.
                  Abrir en el navegador con la IP que muestra el Monitor Serie.
    GET /json   → Respuesta JSON con todos los datos. Útil para dashboards
                  o integrar con otros sistemas sin necesidad de MQTT.

  COMUNICACIÓN MQTT (pub/sub):
  ──────────────────────────────
    PUBLICACIÓN (ESP8266 → Raspberry Pi):
      Tópico : sensors/esp8266
      Cada   : BACKGROUND_SAMPLE_MS milisegundos
      Formato: {"raw":512,"percent":65.3,"state":"WET",
                "watering":false,"cooldown":false}

    SUSCRIPCIÓN (Raspberry Pi → ESP8266):
      Tópico  : commands/esp8266
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

#include <ESP8266WiFi.h>      // Maneja la conexión Wi-Fi del ESP8266.
                               // Permite conectarse a una red, obtener IP, etc.
#include <ESP8266WebServer.h>  // Implementa un servidor HTTP minimalista.
                               // Con él el ESP8266 puede responder a peticiones
                               // GET desde un navegador web.
#include <PubSubClient.h>      // Librería MQTT de Nick O'Leary.
                               // Permite publicar mensajes y suscribirse a tópicos
                               // en un broker MQTT (p. ej. Mosquitto en la RPi).
#include "config.h"            // Archivo de configuración PERSONAL (no subir a git).
                               // Contiene: SSID, contraseña Wi-Fi, pines, umbrales,
                               // tiempos y parámetros MQTT. Créalo copiando
                               // config.example.h y editando con tus valores.

// ================================================================
// OBJETOS GLOBALES
// ================================================================

// ── Servidor web ─────────────────────────────────────────────────
// ESP8266WebServer gestiona peticiones HTTP entrantes.
// El parámetro 80 es el puerto TCP estándar para HTTP.
// Los clientes (navegadores) se conectan a http://<IP_del_ESP>/
ESP8266WebServer server(80);

// ── Clientes MQTT ────────────────────────────────────────────────
// WiFiClient es la capa de transporte TCP/IP sobre Wi-Fi.
// PubSubClient usa ese canal TCP para hablar el protocolo MQTT.
// Separamos los dos objetos para poder reutilizar WiFiClient si
// necesitáramos otras conexiones TCP en el futuro.
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);  // mqtt es el objeto que publica/suscribe

// Temporizador de reconexión MQTT.
// millis() devuelve los ms transcurridos desde el arranque (tipo unsigned long).
// Guardamos el instante del último intento de reconexión para no bloquear
// el loop() mientras esperamos entre reintentos.
unsigned long lastMqttAttemptMs = 0;

// Intervalo mínimo entre intentos de reconexión MQTT (5 segundos).
// UL = unsigned long literal, evita desbordamientos en aritmética de tiempo.
#define MQTT_RECONNECT_INTERVAL_MS 5000UL

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

// ================================================================
// readADC()  —  Lectura promediada del sensor de humedad
// ================================================================
// ¿POR QUÉ PROMEDIAR?
//   El ADC (Convertidor Analógico-Digital) del ESP8266 tiene ruido
//   eléctrico. Una sola lectura puede variar ±10 unidades. Tomar
//   ANALOG_SAMPLES lecturas y promediarlas da un valor más estable.
//
// ¿POR QUÉ delay() ENTRE MUESTRAS?
//   Después de una lectura el condensador interno del ADC necesita
//   un pequeño tiempo para "recargarse". ANALOG_DELAY_MS (5 ms por
//   defecto) evita leer el mismo valor repetido.
//
// RETORNO: entero en el rango 0–1023 (resolución de 10 bits del ADC).
//   • Suelo SECO  → valor ALTO  (poca conductividad → más voltaje → ~571)
//   • Suelo HÚMEDO → valor BAJO (más conductividad → menos voltaje → ~336)
//   (Esto parece contraintuitivo, pero es la lógica del sensor resistivo)
// ================================================================
int readADC() {
  long sum = 0;
  for (int i = 0; i < ANALOG_SAMPLES; i++) {
    sum += analogRead(A0);    // Lee el pin analógico A0 (0–1023)
    delay(ANALOG_DELAY_MS);   // Pequeña pausa para estabilidad del ADC
  }
  return (int)(sum / ANALOG_SAMPLES);  // Devuelve el promedio entero
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
// LED INTEGRADO (active-LOW en NodeMCU):
//   El LED_BUILTIN del NodeMCU está conectado con lógica invertida:
//   digitalWrite(PIN_LED, LOW)  → LED ENCENDIDO (regando)
//   digitalWrite(PIN_LED, HIGH) → LED APAGADO   (en reposo)
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
  // LOW  → LED encendido  (active-low)   cuando estamos en WATERING
  // HIGH → LED apagado    (active-low)   en cualquier otro estado
  digitalWrite(PIN_LED, (relayState == WATERING) ? LOW : HIGH);
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
    "<div class='card'>ADC Raw: <b>" + String(lastRaw) + "</b></div>"
    "<div class='card'>Estado: <b>" + estado + "</b></div>"
    "<div class='card'>Último riego: <b>" + ultimoRiego + "</b></div>"
    "<p><a href='/json'>Ver JSON</a></p>"
    "</body></html>";

  // Enviar la respuesta HTTP 200 OK con el HTML
  // "text/html; charset=UTF-8" es el Content-Type para páginas web
  server.send(200, "text/html; charset=UTF-8", html);
}

// ── GET /json  →  Datos en formato JSON ─────────────────────────
// Útil para dashboards, scripts, o cualquier sistema que consuma datos
// del ESP8266 directamente por HTTP sin pasar por MQTT.
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
  json += "\"raw\":"              + String(lastRaw)                                + ",";
  json += "\"percent\":"          + String(lastPercent, 1)                         + ",";
  json += "\"watering\":"         + String(relayState == WATERING ? "true":"false") + ",";
  json += "\"cooldown\":"         + String(relayState == COOLDOWN ? "true":"false") + ",";
  json += "\"state\":\""          + estado                                         + "\",";
  json += "\"last_watered_sec\":" + String(secsAgo);
  json += "}";

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
//             (ej: "commands/esp8266")
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
      digitalWrite(PIN_LED,   LOW);   // LOW  → LED encendido (active-low)
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
    ok = mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS_BROKER);
  } else {
    ok = mqtt.connect(MQTT_CLIENT_ID);  // Conexión anónima (sin usuario/contraseña)
  }

  if (ok) {
    Serial.println(" OK");

    // Suscribirse al tópico de comandos para recibir órdenes remotas.
    // QoS 0 (por defecto en PubSubClient): fire-and-forget, sin confirmación.
    // Suficiente para comandos de riego donde la latencia importa poco.
    mqtt.subscribe(MQTT_TOPICO_CMD);
    Serial.printf("[MQTT] Suscrito a %s\n", MQTT_TOPICO_CMD);
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

// ================================================================
// publicarMQTT()  —  Envío de datos del sensor al broker
// ================================================================
// Publica el estado completo del sistema en el tópico MQTT_TOPICO.
// La Raspberry Pi (con mqtt_client.py corriendo) recibe este mensaje,
// lo parsea y lo almacena en la base de datos para dashboards.
//
// FORMATO DEL JSON PUBLICADO:
//   {
//     "raw"      : 450,       ← valor crudo del ADC (0–1023)
//     "percent"  : 51.5,      ← humedad en % (0.0–100.0)
//     "watering" : false,     ← true si la válvula está abierta ahora
//     "cooldown" : false,     ← true si está en período de espera
//     "state"    : "WET"      ← "DRY", "WET", "WATERING" o "COOLDOWN"
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

  // Construir el JSON como String de Arduino
  // c_str() convierte String de Arduino a cadena C (const char*) que
  // necesita mqtt.publish()
  String json = "{";
  json += "\"raw\":"       + String(lastRaw)                                + ",";
  json += "\"percent\":"   + String(lastPercent, 1)                         + ",";
  json += "\"watering\":"  + String(relayState == WATERING ? "true":"false") + ",";
  json += "\"cooldown\":"  + String(relayState == COOLDOWN ? "true":"false") + ",";
  json += "\"state\":\""   + estado                                         + "\"";
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
// al encender o resetear el ESP8266. Aquí configuramos todo antes
// de entrar al bucle principal (loop()).
// ================================================================
void setup() {
  // ── Monitor Serie ─────────────────────────────────────────────
  // Inicia la comunicación serial a 115200 baudios.
  // Esto nos permite ver mensajes de depuración en el Monitor Serie
  // de Arduino IDE (Herramientas → Monitor Serie → 115200 baud).
  Serial.begin(115200);
  Serial.println("\n[INICIO] humedadSueloK8");

  // ── Configuración de pines ────────────────────────────────────
  // pinMode() define si un pin es entrada (INPUT) o salida (OUTPUT).
  pinMode(PIN_DO,    INPUT);   // DO del sensor: lectura digital (informativo)
  pinMode(PIN_RELAY, OUTPUT);  // Control del relé: salida digital
  pinMode(PIN_LED,   OUTPUT);  // LED integrado: salida digital

  // ── Estado seguro al arranque ─────────────────────────────────
  // Al arrancar siempre ponemos el relé inactivo para evitar que la
  // válvula quede abierta por un reinicio inesperado del ESP8266.
  digitalWrite(PIN_RELAY, LOW);   // LOW → relé inactivo → válvula CERRADA (seguro)
  digitalWrite(PIN_LED,   HIGH);  // HIGH → LED apagado (active-low: HIGH = apagado)

  // ── Conexión Wi-Fi ────────────────────────────────────────────
  // WiFi.begin() inicia el proceso de conexión en segundo plano.
  // El while() espera activamente hasta tener conexión.
  // WL_CONNECTED es una constante del SDK que indica éxito.
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WIFI] Conectando");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");  // Imprime un punto cada 500ms como indicador de progreso
  }
  Serial.println();
  Serial.print("[WIFI] Conectado. IP: ");
  Serial.println(WiFi.localIP());  // La IP local para acceder al servidor web

  // ── Servidor web ──────────────────────────────────────────────
  // server.on() asocia una URL con su función manejadora (handler).
  // server.begin() pone el servidor en escucha en el puerto 80.
  server.on("/",     handleRoot);  // GET / → página HTML
  server.on("/json", handleJson);  // GET /json → datos JSON
  server.begin();
  Serial.println("[WEB] Servidor iniciado en puerto 80");

  // ── MQTT ──────────────────────────────────────────────────────
  // Solo configuramos MQTT si el usuario definió un servidor en config.h.
  // Si MQTT_SERVER es "" → toda la lógica MQTT se omite silenciosamente.
  if (strlen(MQTT_SERVER) > 0) {
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);   // IP y puerto del broker
    mqtt.setCallback(mqttCallback);           // Función que recibirá los mensajes entrantes
    reconnectMQTT();                          // Primer intento de conexión
  }

  // ── Primera lectura del sensor ────────────────────────────────
  // Hacemos una lectura inicial para que el servidor web tenga datos
  // reales desde el primer momento (en lugar de mostrar 0%).
  lastRaw      = readADC();
  lastPercent  = rawToPercent(lastRaw);
  lastSampleMs = millis();  // Registrar el instante de esta primera lectura
  Serial.printf("[ADC] Raw: %d | Humedad: %.1f%%\n", lastRaw, lastPercent);
}

// ================================================================
// loop()  —  Bucle principal (se ejecuta CONTINUAMENTE)
// ================================================================
// En Arduino, loop() es el corazón del programa. Se llama repetidamente
// sin parar mientras el ESP8266 está encendido.
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
    unsigned long now = millis();

    // Si la conexión se perdió, intentar reconectar cada 5 segundos.
    // Usamos el patrón "non-blocking retry" con timestamp:
    //   - Guardamos cuándo fue el último intento (lastMqttAttemptMs)
    //   - Solo reintentamos si pasaron >= MQTT_RECONNECT_INTERVAL_MS ms
    if (!mqtt.connected() && (now - lastMqttAttemptMs >= MQTT_RECONNECT_INTERVAL_MS)) {
      lastMqttAttemptMs = now;
      reconnectMQTT();
    }

    // mqtt.loop() es OBLIGATORIO en cada iteración cuando se usa PubSubClient.
    // Procesa los mensajes entrantes (llama a mqttCallback si llegó algo)
    // y envía los keepalive MQTT para que el broker no cierre la conexión.
    mqtt.loop();
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

  // ── 4. Control del relé y LED según el estado de humedad ──────
  // updateRelay() evalúa la máquina de estados con el porcentaje actual
  // y decide si debe abrir/cerrar la válvula y encender/apagar el LED.
  // Se llama en CADA iteración del loop() (no solo cuando hay nueva lectura)
  // para que las transiciones de estado (ej: fin del tiempo de riego)
  // se detecten con precisión temporal, sin esperar al siguiente muestreo.
  updateRelay(lastPercent);
}
