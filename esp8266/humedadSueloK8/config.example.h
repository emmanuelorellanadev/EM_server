#pragma once
/*
  config.example.h — Plantilla de configuración para humedadSueloK8
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
// El ESP8266 se conecta a tu red doméstica/empresarial por Wi-Fi.
// Reemplaza los valores entre comillas con los de tu red real.
// SSID: nombre de la red (el que aparece al buscar Wi-Fi en tu celular)
// PASS: contraseña de la red (distingue mayúsculas/minúsculas)
#define WIFI_SSID "TU_RED_WIFI"      // Nombre de tu red (SSID)
#define WIFI_PASS "TU_CONTRASENA"    // Contraseña de tu red

// ================================================================
// Pines  —  Conexión física ESP8266 ↔ componentes
// ================================================================
// IMPORTANTE: Usamos la numeración GPIO real, NO la etiqueta impresa
// en la placa (D1, D5, etc.). La tabla de conversión es:
//   D0=16  D1=5  D2=4  D3=0  D4=2  D5=14  D6=12  D7=13  D8=15
//
// Para identificar tus pines: busca "NodeMCU V3 pinout" en imágenes.

// ── Salida digital del sensor K8/C11 ─────────────────────────────
// El sensor tiene dos salidas:
//   AO (Analog Output)  → conectar a A0 (único pin ADC del ESP8266)
//   DO (Digital Output) → conectar aquí (PIN_DO)
// DO genera HIGH/LOW según un potenciómetro de umbral en el sensor.
// En este firmware solo usamos DO de forma informativa; el control
// real del riego viene de la lectura analógica (más precisa).
// D5 en NodeMCU V3 = GPIO 14
#define PIN_DO 14

// ── Pin de control del módulo relé ───────────────────────────────
// El relé es un interruptor electromecánico controlado eléctricamente.
// Permite que el ESP8266 (3.3 V, señales débiles) controle cargas de
// alto voltaje (12 V DC de la electroválvula) de forma segura.
//
// Este módulo relé es ACTIVE-HIGH:
//   digitalWrite(PIN_RELAY, HIGH) → bobina energizada → contacto NO CIERRA
//                                 → electroválvula recibe 12 V → AGUA FLUYE
//   digitalWrite(PIN_RELAY, LOW)  → bobina apagada → contacto NO ABRE
//                                 → electroválvula sin corriente → SIN AGUA
//                                 → ESTADO SEGURO AL ARRANQUE
//
// D1 en NodeMCU V3 = GPIO 5
#define PIN_RELAY 5

// ── LED integrado del NodeMCU ─────────────────────────────────────
// NodeMCU tiene un LED azul/verde soldado en la placa conectado al
// GPIO2 (D4). Es ACTIVE-LOW, lo que significa lógica invertida:
//   digitalWrite(PIN_LED, LOW)  → LED ENCENDIDO (indica que está regando)
//   digitalWrite(PIN_LED, HIGH) → LED APAGADO
// LED_BUILTIN es una constante predefinida que apunta al pin correcto
// según la placa seleccionada en Arduino IDE.
#define PIN_LED LED_BUILTIN

// ================================================================
// Calibración ADC  ← ¡AJUSTA ESTO SEGÚN TU SENSOR ESPECÍFICO!
// ================================================================
// El ADC (Convertidor Analógico-Digital) del ESP8266 lee voltajes
// en el pin A0 y los convierte a un número entero entre 0 y 1023.
//
// El sensor de humedad genera un voltaje que varía según la humedad:
//   Suelo SECO  → alta resistencia → más voltaje → ADC da valor ALTO (~571)
//   Suelo HÚMEDO → baja resistencia → menos voltaje → ADC da valor BAJO (~336)
//
// PROCEDIMIENTO DE CALIBRACIÓN (hazlo una sola vez):
//   1. Conecta el sensor al ESP8266 y abre el Monitor Serie (115200 baud).
//   2. Pon el sensor en el AIRE (completamente seco): anota el valor Raw.
//      → Ese valor es tu RAW_DRY.
//   3. Sumerge el sensor en un vaso de AGUA: anota el valor Raw.
//      → Ese valor es tu RAW_WET.
//   4. Reemplaza los valores de ejemplo abajo con los tuyos.
//
// ⚠ Los valores aquí son de EJEMPLO con un K8/C11 genérico.
//   Tu sensor puede dar valores diferentes. Usar valores incorrectos
//   produce lecturas erróneas y puede causar riegos indeseados.
#define RAW_DRY 571   // ADC cuando el sensor está completamente seco (en aire)
#define RAW_WET 336   // ADC cuando el sensor está completamente húmedo (en agua)

// ================================================================
// Umbrales de humedad  (porcentaje 0–100 %)
// ================================================================
// Definen cuándo el sistema considera que el suelo está "seco" y
// necesita riego, y cuándo está "húmedo" y no necesita nada.

// UMBRAL DE ACTIVACIÓN DEL RIEGO:
//   Si humedad < ON_THRESHOLD_PERCENT → el suelo está SECO → activar válvula.
//   Ejemplo: con 35, si la humedad cae a 34% → inicia el riego.
//   ⚠ Un valor demasiado bajo = riega poco (la planta puede secarse).
//     Un valor demasiado alto = riega demasiado (puede pudrir las raíces).
#define ON_THRESHOLD_PERCENT  35

// UMBRAL INFORMATIVO SUPERIOR:
//   Se usa solo para mostrar el estado "HUMEDO" en la página web y serial.
//   NO controla el relé directamente (el riego siempre dura RELAY_ON_TIME_MS).
//   Ejemplo: con 45, si la humedad es 46% → muestra "HUMEDO" en la web.
#define OFF_THRESHOLD_PERCENT 45

// ================================================================
// Tiempos del riego  —  Duración y espera entre ciclos
// ================================================================

// DURACIÓN DE CADA CICLO DE RIEGO (en milisegundos):
//   Tiempo que permanece ABIERTA la electroválvula en cada ciclo.
//   El sistema cierra la válvula automáticamente al cumplirse este tiempo.
//
//   Cómo calcular el valor correcto para tu instalación:
//     1. Mide cuántos litros entrega tu gotero/electroválvula por segundo.
//     2. Estima cuánta agua necesita tu maceta/zona de riego por ciclo.
//     3. tiempo_ms = (agua_litros / caudal_litros_por_segundo) × 1000
//
//   Ejemplo: 0.2 L/s de caudal, 1 L por ciclo → 1/0.2 × 1000 = 5000 ms
//
//   UL al final = "Unsigned Long" literal. Necesario porque RELAY_ON_TIME_MS
//   puede superar el rango de un int (32767) en instalaciones con goteo lento.
#define RELAY_ON_TIME_MS   5000UL   // 5 segundos (valor inicial de ejemplo)

// TIEMPO DE ESPERA ENTRE RIEGOS CONSECUTIVOS (cooldown):
//   Después de regar, el sistema espera este tiempo antes de permitir
//   otro ciclo de riego aunque el sensor siga detectando suelo seco.
//   Esto es necesario porque:
//   • El suelo tarda en absorber el agua y el sensor en detectar el cambio.
//   • Sin cooldown, el sistema podría regar en bucle continuo.
//   Recomendación: al menos 2–5 veces RELAY_ON_TIME_MS para suelos normales.
#define COOLDOWN_MS       15000UL   // 15 segundos (ajustar según tipo de suelo)

// ================================================================
// Lectura ADC  —  Parámetros de estabilidad de señal
// ================================================================
// El ADC del ESP8266 no es de alta precisión y tiene ruido eléctrico.
// Promediar varias muestras mejora significativamente la estabilidad
// de las lecturas sin necesidad de hardware adicional.

// Cuántas lecturas se promedian en cada ciclo de muestreo.
// Más muestras = más estable pero más lento. 20 es un buen balance.
// Con 20 muestras × 5 ms = 100 ms por lectura completa.
#define ANALOG_SAMPLES   20

// Tiempo de espera entre lecturas consecutivas del ADC (en ms).
// Da tiempo al circuito de muestreo interno del ADC para recargarse.
// Sin esta pausa, las lecturas sucesivas pueden ser casi idénticas
// porque el capacitor interno aún no se ha cargado completamente.
#define ANALOG_DELAY_MS   5

// ================================================================
// Muestreo en segundo plano  —  Frecuencia de lectura del sensor
// ================================================================
// Cada cuántos milisegundos se lee el sensor, se actualiza el control
// de riego y se publican los datos por MQTT.
//
// Consideraciones para elegir el valor:
//   • Muy frecuente (< 1000 ms): más datos pero mayor consumo de CPU/red.
//     Puede saturar la base de datos de la Raspberry Pi innecesariamente.
//   • Poco frecuente (> 30000 ms): respuesta lenta ante cambios de humedad.
//   • 3000–5000 ms: balance óptimo para monitoreo de humedad de suelo
//     (la humedad cambia lentamente, no necesita muestreo muy rápido).
#define BACKGROUND_SAMPLE_MS  3000UL   // 3 segundos entre lecturas

// ================================================================
// MQTT  —  Comunicación con la Raspberry Pi vía broker
// ================================================================
// MQTT (Message Queuing Telemetry Transport) es un protocolo de
// mensajería ligero basado en el patrón publicar/suscribir (pub/sub).
// Funciona sobre TCP/IP con un servidor central llamado "broker"
// (en este proyecto: Mosquitto corriendo en la Raspberry Pi).
//
// VENTAJAS DE MQTT PARA IoT:
//   • Muy bajo consumo de ancho de banda (cabecera mínima de 2 bytes)
//   • Tolerante a redes inestables (reconexión automática)
//   • Desacopla productores y consumidores: el ESP8266 publica datos
//     sin saber quién los lee; la RPi los recibe sin saber cuántos
//     dispositivos los generan.
//
// CÓMO FUNCIONA EN ESTE PROYECTO:
//
//   PUBLICACIÓN (ESP8266 → Raspberry Pi):
//     El ESP8266 envía lecturas del sensor cada BACKGROUND_SAMPLE_MS.
//     Tópico: sensors/esp8266
//     Ejemplo: {"raw":450,"percent":51.5,"state":"WET",
//               "watering":false,"cooldown":false}
//     La Raspberry Pi (mqtt_client.py) escucha "sensors/#" y guarda
//     cada mensaje en la base de datos SQLite.
//
//   SUSCRIPCIÓN (Raspberry Pi → ESP8266):
//     El ESP8266 escucha comandos de riego remoto.
//     Tópico: commands/esp8266
//     Comando: {"action":"water"}
//     Efecto: activa el relé y enciende el LED, igual que si el
//             sensor detectara suelo seco localmente.
//
// GLOSARIO:
//   BROKER    : servidor central que recibe y distribuye mensajes
//   TÓPICO    : nombre jerárquico del canal (ej: "sensors/esp8266")
//   QoS       : calidad de servicio (0 = sin confirmación, 1 = con confirmación)
//   CLIENT_ID : nombre único de este dispositivo ante el broker
//
// ¿CÓMO DESHABILITAR MQTT?
//   Deja MQTT_SERVER vacío: #define MQTT_SERVER ""
//   El firmware omitirá toda la lógica MQTT automáticamente.
//
// Instala la librería "PubSubClient" de Nick O'Leary desde el
// Library Manager de Arduino IDE antes de compilar.

// IP del broker MQTT (tu Raspberry Pi en la red local).
// ⚠ Cambia "192.168.1.2" por la IP real de tu Raspberry Pi.
//   Puedes verla con: hostname -I  (en la terminal de la RPi)
//   Dejar "" para deshabilitar MQTT completamente.
#define MQTT_SERVER      "192.168.1.2"         // IP de la Raspberry Pi (broker)

// Puerto TCP del broker. 1883 es el estándar MQTT sin cifrado TLS.
// Si tu broker usa TLS (cifrado), el puerto es generalmente 8883.
#define MQTT_PORT        1883                   // Puerto TCP estándar sin TLS

// Identificador único de ESTE dispositivo ante el broker.
// Si dos dispositivos usan el mismo CLIENT_ID, el broker desconecta
// al anterior. Cambia este nombre si tienes varios ESP8266.
#define MQTT_CLIENT_ID   "esp8266-invernadero" // ID único por dispositivo

// Tópico de PUBLICACIÓN: el ESP8266 envía sus datos aquí.
// La Raspberry Pi (mqtt_client.py) está suscrita a "sensors/#"
// para recibir datos de todos los dispositivos del invernadero.
#define MQTT_TOPICO      "sensors/esp8266"

// Tópico de SUSCRIPCIÓN: el ESP8266 escucha comandos aquí.
// El dashboard de la Raspberry Pi publica {"action":"water"} en
// este tópico para activar el riego de forma remota.
#define MQTT_TOPICO_CMD  "commands/esp8266"

// Credenciales de autenticación del broker MQTT.
// Dejar ambas vacías ("") si el broker Mosquitto no requiere usuario/contraseña.
// Para configurar autenticación en Mosquitto: ver mosquitto_passwd.
#define MQTT_USER        ""    // Usuario MQTT (dejar "" si no hay autenticación)
#define MQTT_PASS_BROKER ""    // Contraseña MQTT (dejar "" si no hay autenticación)
