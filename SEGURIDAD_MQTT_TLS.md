# Seguridad MQTT y TLS en EM_server

## 1) Objetivo de este documento

Este documento explica, de forma didactica y aplicada a este proyecto, como
asegurar la comunicacion MQTT para que:

1. Los mensajes no puedan leerse (confidencialidad).
2. No puedan alterarse en transito (integridad).
3. Emisor y receptor se autentiquen correctamente (handshake y confianza).

Incluye teoria y pasos practicos para implementar TLS/mTLS en `EM_server`.

---

## 2) Analisis del estado actual del codigo

### Hallazgos principales

- `config.json` usa `mqtt.port = 1883` y credenciales vacias.
- `em_server/services/mqtt_service.py` conecta con `client.connect(...)` sin `tls_set(...)`.
- `em_server/services/sensehat_service.py` tambien conecta sin TLS.
- `em_server/routes/api.py` publica comandos con `paho.mqtt.publish.single(...)` sin bloque `tls`.
- En ESP8266 (`config.example.h`) el default es `MQTT_PORT 1883`.

### Implicacion

Actualmente el canal MQTT es funcional, pero en texto plano o con seguridad
basica. Eso permite escenarios de sniffing, MITM y publicacion no autorizada
si la red local se ve comprometida.

---

## 3) Parte teorica (didactica)

## 3.1 Que protege TLS en MQTT

MQTT por si solo no cifra datos. TLS agrega:

- Cifrado: evita leer paquetes capturados.
- Integridad: detecta alteraciones del mensaje.
- Autenticacion del servidor: el cliente valida el certificado del broker.

## 3.2 Que es el handshake TLS

Piensalo como 2 capas: identidad + canal seguro.

Flujo simplificado y correcto:

1. `ClientHello`: el cliente propone version TLS, algoritmos y parametros.
2. `ServerHello`: el broker elige algoritmos y envia su certificado (`broker.crt`).
3. El cliente valida ese certificado con su CA de confianza (`ca.crt`):
   - cadena de firma,
   - fecha de validez,
   - nombre (CN/SAN) esperado.
4. Se realiza intercambio de claves efimeras (normalmente ECDHE) para crear
   una clave de sesion simetrica.
5. A partir de ahi, MQTT viaja cifrado con clave simetrica (mas rapido).

Si activas **mTLS**, ademas:

6. El broker pide certificado al cliente.
7. El cliente envia su certificado (`client.crt`) y prueba posesion de su
   clave privada (`client.key`) durante el handshake.
8. El broker valida firma/fecha/CA y decide si acepta al cliente.

Resultado:

- TLS: el cliente sabe que habla con el broker legitimo.
- mTLS: broker y cliente saben con quien hablan.

## 3.3 Clave publica y privada (explicacion clara)

Cada identidad tiene un par de claves:

- Clave privada (`.key`): secreta, nunca se comparte.
- Clave publica: se incluye en el certificado (`.crt`).

Relaciones importantes:

1. Lo que cifras con la publica solo lo abre la privada.
2. Lo que firmas con la privada se valida con la publica.

En TLS moderno, la mayor parte del trafico usa cifrado simetrico de sesion.
Las claves publica/privada se usan para autenticar y acordar esa sesion.

## 3.4 Que significa cada archivo (`.crt`, `.key`, `ca`, `broker`)

Tabla rapida:

- `ca.key`: clave privada de la Autoridad Certificadora (CA). Es la mas critica.
- `ca.crt`: certificado publico de la CA. Se distribuye a clientes/broker para validar firmas.
- `broker.key`: clave privada del broker Mosquitto.
- `broker.crt`: certificado del broker firmado por la CA.
- `client.key`: clave privada de un cliente (ej. `mqtt_client.key` para el suscriptor).
- `client.crt`: certificado del cliente firmado por la CA.
- `*.csr`: Certificate Signing Request; solicitud de firma de certificado.
- `ca.srl`: archivo de serial que OpenSSL usa al firmar multiples certificados.

Regla operativa:

- `.key` se protege con permisos estrictos.
- `.crt` y `ca.crt` pueden distribuirse segun necesidad.

Permisos recomendados en Linux:

```bash
chmod 600 /etc/mosquitto/certs/*.key
chmod 644 /etc/mosquitto/certs/*.crt
chown mosquitto:mosquitto /etc/mosquitto/certs/broker.key /etc/mosquitto/certs/broker.crt
```

## 3.5 TLS vs mTLS

- TLS normal: cliente autentica al broker.
- mTLS: cliente y broker se autentican mutuamente con certificados.

Para IoT, mTLS es ideal en nodos criticos (Raspberry, servicios Python,
gateways). En microcontroladores limitados puede requerir ajustes por RAM.

## 3.6 Como se realiza la autenticacion en la practica

Hay 3 controles acumulativos (se recomiendan juntos):

1. Autenticacion TLS del servidor (siempre):
   - El cliente valida `broker.crt` contra `ca.crt`.
2. Autenticacion TLS del cliente (mTLS, opcional/ideal):
   - El broker valida `client.crt` contra `ca.crt`.
3. Autenticacion MQTT aplicativa:
   - `username/password` + ACL por topico.

Interpretacion:

- TLS/mTLS valida identidad criptografica y protege el canal.
- Usuario/password y ACL define autorizacion funcional (que topicos puede usar).
- No son excluyentes; se complementan.

## 3.7 Diagrama de secuencia (TLS normal)

```text
Cliente MQTT                          Broker Mosquitto                       CA
     |                                       |                               |
     | ----------- ClientHello ----------->  |                               |
     | <---------- ServerHello ------------  |                               |
     | <---------- broker.crt -------------  |                               |
     |                                       |                               |
     | ---- valida broker.crt con ca.crt ----                               |
     | ---- (firma, fechas, CN/SAN) --------                               |
     |                                       |                               |
     | <------ parametros ECDHE -----------> |                               |
     | ------ clave de sesion simetrica ---- |                               |
     |                                       |                               |
     | <--------- Finished (TLS OK) -------- |                               |
     | --------- Finished (TLS OK) --------> |                               |
     |                                       |                               |
     | ===== MQTT cifrado (CONNECT/PUBLISH/SUBSCRIBE) =====>                |
```

Lectura didactica:

- El broker no "pregunta" en vivo a la CA en cada conexion.
- El cliente valida localmente con `ca.crt` que ya confia previamente.

## 3.8 Diagrama de secuencia (mTLS + usuario/password + ACL)

```text
Cliente MQTT                          Broker Mosquitto                       CA
     |                                       |                               |
     | ----------- ClientHello ----------->  |                               |
     | <---------- ServerHello ------------  |                               |
     | <---------- broker.crt -------------  |                               |
     |                                       |                               |
     | ---- valida broker.crt con ca.crt ----                               |
     |                                       |                               |
     | <------ CertificateRequest ---------  |                               |
     | ----------- client.crt ------------>  |                               |
     | -------- prueba con client.key ---->  |                               |
     |                                       | ---- valida firma cliente --->|
     |                                       | <--- confianza por ca.crt ----|
     |                                       |                               |
     | <--------- Finished (TLS OK) -------- |                               |
     | --------- Finished (TLS OK) --------> |                               |
     |                                       |                               |
     | ---- MQTT CONNECT (username/pass) --> |                               |
     | <---- CONNACK (aceptado/rechazado) -- |                               |
     | ---- MQTT PUBLISH topic X --------->  |                               |
     | <---- ACL permite o deniega --------- |                               |
```

Clave conceptual:

1. mTLS responde "quien eres" (identidad criptografica).
2. Usuario/password responde "con que cuenta entras".
3. ACL responde "que puedes hacer" (topicos permitidos).

## 3.9 Mapa mental rapido de archivos y confianza

```text
ca.key (secreto maximo)  ---> firma --->  broker.crt / client.crt
ca.crt (publico)         ---> valida ---> broker.crt / client.crt

broker.key (secreto broker) + broker.crt ---> identidad del broker
client.key (secreto cliente) + client.crt ---> identidad del cliente
```

Regla de oro:

- Si se filtra un `.key`, esa identidad queda comprometida y debes revocar/rotar.
- Si se filtra un `.crt` publico, no compromete por si solo la identidad.

---

## 4) Arquitectura recomendada para este proyecto

Objetivo recomendado:

1. Broker Mosquitto en `8883` con TLS 1.2+.
2. Usuarios por rol + ACL por topico.
3. Clientes Python con verificacion de CA.
4. mTLS al menos para servicios de servidor.
5. ESP8266: TLS con `WiFiClientSecure` si es viable; si no, red IoT aislada
   + ACL estricta + bridge seguro.

---

## 5) Parte practica (paso a paso)

## Paso A - Crear CA y certificados

Ejemplo local con OpenSSL:

```bash
mkdir -p certs && cd certs

# 1) CA privada
openssl genrsa -out ca.key 4096
openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 -out ca.crt \
  -subj "/CN=EM-Root-CA"

# 2) Cert del broker
openssl genrsa -out broker.key 2048
openssl req -new -key broker.key -out broker.csr -subj "/CN=raspberrypi.local"
openssl x509 -req -in broker.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out broker.crt -days 825 -sha256

# 3) Cert de cliente (ejemplo: mqtt_client)
openssl genrsa -out mqtt_client.key 2048
openssl req -new -key mqtt_client.key -out mqtt_client.csr -subj "/CN=em_server_subscriber"
openssl x509 -req -in mqtt_client.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out mqtt_client.crt -days 825 -sha256
```

> Practica segura: `ca.key` no debe vivir en el mismo host productivo.

Validaciones utiles:

```bash
# Ver contenido legible de un certificado
openssl x509 -in broker.crt -text -noout

# Verificar que broker.crt fue firmado por ca.crt
openssl verify -CAfile ca.crt broker.crt

# Verificar certificado de cliente
openssl verify -CAfile ca.crt mqtt_client.crt
```

## Paso B - Endurecer Mosquitto

Ejemplo de `mosquitto.conf`:

```conf
persistence true

listener 8883
protocol mqtt

allow_anonymous false
password_file /etc/mosquitto/passwd
acl_file /etc/mosquitto/acl

cafile /etc/mosquitto/certs/ca.crt
certfile /etc/mosquitto/certs/broker.crt
keyfile /etc/mosquitto/certs/broker.key

tls_version tlsv1.2

# Activar si quieres mTLS obligatorio
# require_certificate true
# use_identity_as_username true
```

Notas didacticas de estas 2 lineas:

- `require_certificate true`: el broker no acepta clientes sin certificado.
- `use_identity_as_username true`: usa el CN del certificado cliente como
  identidad MQTT (muy util para ACL por certificado).

ACL ejemplo:

```conf
user em_server_subscriber
topic read sensors/#
topic read devices/+/status

user em_server_sensehat
topic write sensors/raspberrypi

user em_server_dashboard
topic write commands/esp8266
```

## Paso C - Extender `config.json`

Propuesta:

```json
"mqtt": {
  "broker": "localhost",
  "port": 8883,
  "username": "em_server_subscriber",
  "password": "***",
  "tls": {
    "enabled": true,
    "ca_cert": "/etc/em/certs/ca.crt",
    "client_cert": "/etc/em/certs/mqtt_client.crt",
    "client_key": "/etc/em/certs/mqtt_client.key",
    "insecure": false
  }
}
```

## Paso D - Cambios en clientes Python

En `em_server/services/mqtt_service.py` y `em_server/services/sensehat_service.py`:

```python
tls_cfg = cfg.get("tls", {})
if tls_cfg.get("enabled"):
    client.tls_set(
        ca_certs=tls_cfg["ca_cert"],
        certfile=tls_cfg.get("client_cert"),
        keyfile=tls_cfg.get("client_key"),
    )
    client.tls_insecure_set(bool(tls_cfg.get("insecure", False)))
```

Importante:

- En produccion, `insecure` debe ser `false`.
- Si pones `true`, desactivas parte de la validacion del certificado y abres
  la puerta a MITM.

En `em_server/routes/api.py` para `mqtt_publish.single(...)`:

```python
tls = None
if mqtt_cfg.get("tls", {}).get("enabled"):
    tls = {
        "ca_certs": mqtt_cfg["tls"]["ca_cert"],
        "certfile": mqtt_cfg["tls"].get("client_cert"),
        "keyfile": mqtt_cfg["tls"].get("client_key"),
        "insecure": bool(mqtt_cfg["tls"].get("insecure", False)),
    }

mqtt_publish.single(..., tls=tls)
```

## Paso E - Handshake de identidad en ESP8266

Si el recurso de memoria lo permite:

1. Cambiar `WiFiClient` por `WiFiClientSecure`.
2. Cargar CA del broker (trust anchor).
3. Usar `MQTT_PORT 8883`.

Si no es viable mTLS en ESP8266:

- Mantener TLS del lado Raspberry/servidor.
- Segmentar red IoT (VLAN/subred separada).
- Restringir broker por ACL y firewall.

## Paso F - Seguridad adicional del endpoint web

Actualmente `POST /api/command/water` no aplica autenticacion fuerte.
Agregar minimo:

1. Header `X-API-Key`.
2. Comparacion con variable de entorno.
3. `401` si no coincide.
4. Rate limit por IP.

---

## 6) Validacion y pruebas

## 6.0 Prueba conceptual del handshake

Si quieres observar el certificado que entrega el broker:

```bash
openssl s_client -connect localhost:8883 -CAfile /etc/em/certs/ca.crt
```

Que debes revisar en la salida:

- `Verify return code: 0 (ok)`
- Subject/SAN coherente con el host del broker
- Fechas de validez correctas

## 6.1 Probar broker TLS desde consola

```bash
mosquitto_pub -h localhost -p 8883 \
  --cafile /etc/em/certs/ca.crt \
  -u em_server_sensehat -P '***' \
  -t sensors/test -m '{"ok":true}'
```

## 6.2 Verificar cifrado en ejecucion

- Confirmar que clientes conectan a `8883`.
- Revisar logs de Mosquitto por fallos de cert/ACL.
- Confirmar que no hay trafico MQTT plano en `1883`.

## 6.3 Prueba de rechazo

- Cliente sin CA valida -> debe fallar.
- Usuario sin ACL adecuada -> debe fallar.
- Cert expirado -> debe fallar.

---

## 7) Plan de implementacion sugerido

Semana 1 (alto impacto, baja complejidad):

1. `allow_anonymous false` + `password_file` + ACL.
2. Proteger endpoint de comando con API key.
3. Mover secretos fuera de `config.json` (variables de entorno).

Semana 2:

1. TLS en broker (`8883`) + clientes Python.
2. Validacion de CA obligatoria.
3. Pruebas de rechazo/aceptacion.

Semana 3:

1. Evaluar mTLS para clientes de servidor.
2. Estrategia para ESP8266 (TLS o red aislada).
3. Rotacion de certificados y credenciales.

---

## 8) Checklist final de seguridad MQTT/TLS

- [ ] Broker solo con `allow_anonymous false`.
- [ ] ACL por topico y por rol.
- [ ] Clientes en `8883` con TLS activo.
- [ ] Verificacion de CA habilitada (`insecure = false`).
- [ ] Endpoint de comandos autenticado.
- [ ] Certificados con expiracion y rotacion definida.
- [ ] Logs y alertas para intentos fallidos.

---

## 9) Conclusiones aplicadas a EM_server

En este proyecto, el salto de seguridad mas importante es migrar de MQTT plano
en `1883` a TLS en `8883` con autenticacion y ACL. El "handshake" que buscas
para validar emisor/receptor se resuelve correctamente con TLS/mTLS; no hace
falta inventar un protocolo propio mientras tengas certificados bien gestionados.

Con eso reduces de forma fuerte el riesgo de lectura, intercepcion y suplantacion
de mensajes MQTT.
