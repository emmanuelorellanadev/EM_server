# Seguridad en EM_server: TLS, mTLS y Criptografía

## Guía Didáctica para Estudiantes de Seguridad e Ingeniería

---

## PARTE 1: ¿Qué problema resuelve todo esto?

### El problema base: comunicación en texto plano

Imagina que tu ESP32 envía temperatura al broker Mosquitto:

```
ESP32 -----"temperatura: 25°C"-----> Broker
```

Sin cifrado, **cualquiera** en la red puede leer ese mensaje:

```
ESP32 -----"temperatura: 25°C"-----> [ hacker escucha ] -----> Broker
                                     "temperatura: 25°C" ✓
```

Pero hay algo peor que escuchar: **suplantar**. Un atacante puede:

```
ESP32 falso ---"regar ahora"---→ Broker ---→ Tu ESP32 real abre el relay
```

El broker no tiene forma de saber si el mensaje viene de tu ESP32 legítimo o de un impostor.

**TLS resuelve estos 3 problemas:**

| Problema | Solución TLS | Analogía |
|----------|-------------|----------|
| Alguien lee tus mensajes | **Cifrado** | Hablar en un idioma que solo entienden el emisor y receptor |
| Alguien modifica tus mensajes | **Integridad** | Sobre con sello de cera que se rompe si lo abren |
| No sabes con quién hablas | **Autenticación** | Mostrar una credencial verificable antes de hablar |

---

## PARTE 2: Los dos tipos de cifrado

### 2.1 - Cifrado Simétrico (misma clave para todo)

```
    Mensaje original: "regar"
         ↓
    Clave secreta: "mi_clave_123"
         ↓
    Mensaje cifrado: "xK#9f$2m"
         ↓
    Clave secreta: "mi_clave_123"  ← MISMA clave
         ↓
    Mensaje original: "regar"
```

**Analogía:** Un candado con la misma llave. Quien tiene la llave puede abrir y cerrar.

**Ventaja:** Es rápido. Una laptop puede cifrar gigabytes por segundo.

**Problema:** ¿Cómo le das la clave al otro lado sin que alguien la intercepte?

```
ESP32 --"mi_clave_123"--> [hacker la ve] --> Broker
          ↑
    ¡Ya no es secreta!
```

### 2.2 - Cifrado Asimétrico (par de claves)

Aquí hay **dos claves** que van juntas pero son diferentes:

```
┌─────────────────────────────────────────────────┐
│           PAR DE CLAVES                         │
│                                                 │
│   CLAVE PÚBLICA  ←→  CLAVE PRIVADA             │
│   (se comparte)        (NUNCA se comparte)      │
│                                                 │
│   Lo que cifra con la pública,                  │
│   solo lo descifra la privada.                  │
│                                                 │
│   Lo que firma con la privada,                  │
│   se verifica con la pública.                   │
└─────────────────────────────────────────────────┘
```

**Analogía:** Un buzón de correo. cualquiera puede meter cartas (clave pública), pero solo el dueño puede abrirla (clave privada).

**En EM_server:**

```
                    CLAVE PÚBLICA              CLAVE PRIVADA
                    (ca.crt)                   (ca.key)
                         │                          │
    Mosquitto broker ────┤                          │
                         │                          │
    ESP32 ───────────────┤                          │
                         │                          │
    Servidor Python ─────┤                          │
                         │                          │
                    Todos la tienen           Solo la CA la tiene
```

**Ventaja:** Resuelve el problema de intercambio de claves.

**Desventaja:** Es lento (1000x más que simétrico). Por eso TLS usa ambos: asimétrico para **acordar** una clave, y simétrico para **cifrar** la comunicación.

---

## PARTE 3: SHA-256 - El Hash Cryptográfico

### 3.1 - ¿Qué es un hash?

Un hash es una función que toma **cualquier cantidad de datos** y produce una **huella digital fija** de longitud determinada.

```
Entrada: "Hola mundo"
Salida:  315f5d2f28e0b879ea0f3d568e3a1f37c7f8f1e4b8a2c5d6e7f8a9b0c1d2e3f4

Entrada: "Hola mundp"  (cambio de UN solo carácter)
Salida:  8a7b6c5d4e3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d0e9f8a7b
         ↑ Completamente diferente
```

### 3.2 - Propiedades fundamentales de SHA-256

#### Propiedad 1: Determinismo

Siempre produce el mismo hash para la misma entrada:

```
SHA-256("temperatura: 25°C") → "a1b2c3d4..." (SIEMPRE)
SHA-256("temperatura: 25°C") → "a1b2c3d4..." (SIEMPRE)
```

#### Propiedad 2: Avalanche Effect (Efecto Avalancha)

Un cambio mínimo en la entrada produce un cambio radical en la salida:

```
SHA-256("A") → "559aead08264d5795d3909718cdd05abd49572eaaa435a2c85e2d60e39b27830"
SHA-256("B") → "05270fa8412369a774af34969226723b8b2ab1585d858af0948a6366cc83b963"
                     ↑ Totalmente diferente, aunque solo cambió 1 bit
```

Esto es intencional. Si un atacante modificara un solo byte de un mensaje firmado, el hash cambiaría completamente y la firma sería inválida.

#### Propiedad 3: One-Way (Irreversible)

No puedes recuperar el mensaje original a partir del hash:

```
SHA-256("mensaje_secreto") → "a1b2c3d4..."
"a1b2c3d4..." → ???  (IMPOSIBLE saber que el mensaje era "mensaje_secreto")
```

No existe inversa matemática. La única forma es probar combinaciones hasta encontrar una que produzca el mismo hash (fuerza bruta).

#### Propiedad 4: Resistencia a Colisiones

Es computacionalmente imposible encontrar dos mensajes diferentes con el mismo hash:

```
¿Existe "X" tal que SHA-256("X") == SHA-256("mensaje_secreto")?
→ Sí, pero encontrarlo requeriría 2^128 intentos en promedio
→ A 1 billion de intentos por segundo: ~10^22 años
→ El universo tiene ~1.4 × 10^10 años
```

#### Propiedad 5: Longitud Fija de Salida

Siempre produce 256 bits (32 bytes) = 64 caracteres hexadecimales:

```
SHA-256("a")      → 32 bytes
SHA-256("aaaa..." × 1 millón) → 32 bytes (misma longitud SIEMPRE)
```

### 3.3 - ¿Cómo funciona internamente SHA-256?

SHA-256 es un algoritmo de la familia Merkle-Damgård. Funciona así:

```
┌─────────────────────────────────────────────────────────────┐
│                    PROCESO SHA-256                           │
│                                                             │
│  Mensaje: "Hola"                                            │
│                                                             │
│  1. PADDING (relleno)                                       │
│     "Hola" → "01001000 01101111 01101100 01100001           │
│               10000000 00000000 ... 00000000 00110000"      │
│     (se añade un 1, ceros, y el tamaño al final)            │
│                                                             │
│  2. DIVIDIR en bloques de 512 bits                          │
│     Bloque 1: [datos]                                       │
│     Bloque 2: [datos]                                       │
│     ...                                                     │
│                                                             │
│  3. INICIALIZAR valores hash (constantes fijas)              │
│     h0 = 0x6a09e667                                        │
│     h1 = 0xbb67ae85                                        │
│     h2 = 0x3c6ef372                                        │
│     h3 = 0xa54ff53a                                        │
│     h4 = 0x510e527f                                        │
│     h5 = 0x9b05688c                                        │
│     h6 = 0x1f83d9ab                                        │
│     h7 = 0x5be0cd19                                        │
│                                                             │
│  4. PARA CADA BLOQUE, ejecutar 64 rondas de compresión     │
│     Cada ronda usa operaciones bitwise:                     │
│     - AND, OR, XOR, NOT                                     │
│     - Rotaciones (ROT)                                      │
│     - Shifts (desplazamientos)                              │
│     - Suma modular (mod 2^32)                               │
│                                                             │
│     Ronda 1:  h0' = h0 + Σ0(h0) + Ch(h4,h5,h6) + K1 + W1  │
│     Ronda 2:  h1' = h1 + Σ1(h0) + Maj(h0,h1,h2) + K2 + W2  │
│     ...                                                     │
│     Ronda 64: resultado final                               │
│                                                             │
│  5. CONCATENAR los 8 valores finales                        │
│     h0 || h1 || h2 || h3 || h4 || h5 || h6 || h7            │
│                                                             │
│  6. SALIDA: 256 bits = 64 caracteres hex                    │
│     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"│
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 3.4 - Operaciones bitwise usadas en SHA-256

```python
# AND - solo 1 si ambos bits son 1
A & B:  1100 & 1010 = 1000

# OR - 1 si al menos uno es 1
A | B:  1100 | 1010 = 1110

# XOR - 1 si son diferentes (útil para detectar cambios)
A ^ B:  1100 ^ 1010 = 0110

# NOT - invierte todos los bits
~A:     ~1100 = 0011

# Rotación - mueve bits hacia la izquierda, los que salen reaparecen a la derecha
ROT(a, n): 0110 → 1100 (rotar 1 bit a la izquierda)

# Desplazamiento - mueve bits, los que salen se pierden
SHR(a, n): 0110 → 0011 (desplazar 1 bit a la derecha)
```

### 3.5 - Por qué SHA-256 es seguro

| Ataque | Dificultad | Explicación |
|--------|-----------|-------------|
| Fuerza bruta (preimage) | 2^256 intentos | Probar todas las entradas posibles |
| Colisión (birthday attack) | 2^128 intentos | Encontrar dos entradas con mismo hash |
| Segundo preimage | 2^256 intentos | Dada una entrada, encontrar otra con mismo hash |

A 2^256, incluso con una supercomputadora de 10^18 operaciones por segundo, tomaría más tiempo que la edad del universo.

### 3.6 - SHA-256 en el contexto de TLS/mTLS

SHA-256 se usa en múltiples partes del proceso:

```
┌──────────────────────────────────────────────────────────────┐
│ USOS DE SHA-256 EN mTLS                                      │
│                                                              │
│ 1. FIRMA DE CERTIFICADOS                                     │
│    La CA hashea el contenido del certificado con SHA-256     │
│    y luego cifra el hash con su clave privada (RSA)          │
│                                                              │
│    Contenido cert → SHA-256 → hash → RSA(hash, ca.key)      │
│    → firma digital                                           │
│                                                              │
│ 2. INTEGRIDAD DEL HANDSHAKE                                  │
│    Al final del handshake TLS, ambos lados calculan          │
│    un hash de TODOS los mensajes intercambiados.             │
│    Si alguien modificó un mensaje, el hash no coincide.      │
│                                                              │
│ 3. PRUEBA DE INTEGRIDAD (Finished message)                   │
│    Cliente: SHA-256(todos los mensajes) → lo envía           │
│    Servidor: calcula el mismo hash → compara                 │
│    Si coinciden → el handshake no fue alterado               │
│                                                              │
│ 4. HMAC (Hash-based Message Authentication Code)             │
│    HMAC-SHA-256(key, message) = SHA-256(key ⊕ opad          │
│                                       ∥ SHA-256(key ⊕ ipad  │
│                                                 ∥ message))  │
│    Usado para autenticar mensajes TLS                        │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 3.7 - Diferencia entre SHA-256 y SHA-3/MD5

| Algoritmo | Tamaño hash | Seguridad | Estado |
|-----------|------------|-----------|--------|
| MD5 | 128 bits | Roto (colisiones encontradas) | **NO USAR** |
| SHA-1 | 160 bits | Debilitado (colisiones prácticas) | **NO USAR** |
| SHA-256 | 256 bits | Seguro (sin colisiones conocidas) | **USAR** |
| SHA-512 | 512 bits | Seguro (más margen de seguridad) | USAR para datos críticos |
| SHA3-256 | 256 bits | Seguro (diseño diferente, más moderno) | USAR |

### 3.8 - Práctica: Calcular SHA-256 manualmente

```bash
# En Linux/Mac:
echo -n "Hola mundo" | shasum -a 256
# Salida: 315f5d2f28e0b879ea0f3d568e3a1f37c7f8f1e4b8a2c5d6e7f8a9b0c1d2e3f4  -

# Con OpenSSL:
echo -n "Hola mundo" | openssl dgst -sha256
# Salida: SHA256(stdin)= 315f5d2f28e0b879ea0f3d568e3a1f37c7f8f1e4b8a2c5d6e7f8a9b0c1d2e3f4

# Ver el hash de un archivo:
shasum -a 256 mi_archivo.txt

# Verificar un hash:
echo "hash_esperado  mi_archivo.txt" | shasum -a 256 -c
```

```python
# En Python:
import hashlib
h = hashlib.sha256()
h.update(b"Hola mundo")
print(h.hexdigest())

# O de una sola línea:
hashlib.sha256(b"Hola mundo").hexdigest()
```

### 3.9 - Analogía final de SHA-256

Piensa en SHA-256 como un **molino industrial**:

```
    Cualquier cosa ──► MOLINO ──► Harina de longitud fija

    "Hola" ──► SHA-256 ──► "e3b0c44298fc1c149a..."
    "Hola mundo" ──► SHA-256 ──► "315f5d2f28e0b879..."
    Un video de 4K ──► SHA-256 ──► "9f86d081884c..."  (misma longitud)
```

**Propiedades del molino:**
1. Siempre produce la misma harina para la misma masa (determinista)
2. Un grano de arena en la masa produce harina completamente diferente (avalancha)
3. No puedes reconstruir la masa a partir de la harina (one-way)
4. Es imposible encontrar dos masas distintas que produzcan la misma harina (colisión)

---

## PARTE 4: ¿Qué es un certificado?

Un certificado es como un **pasaporte digital**. Contiene:

```
┌──────────────────────────────────────────────┐
│           CERTIFICADO (ca.crt)               │
│                                              │
│  Nombre:        EM-Root-CA                   │
│  Organización:  EM-Project                   │
│  Válido desde:  2026-01-01                   │
│  Válido hasta:  2036-01-01                   │
│  Clave pública: [datos criptográficos]       │
│  Firma de:      [la propia CA, auto-firmado] │
│                                              │
└──────────────────────────────────────────────┘
```

### 4.1 - La Autoridad Certificadora (CA)

La CA es una entidad de **confianza**. Piensa en ella como una oficina de pasaportes:

```
                    ┌──────────────┐
                    │   EM-Root-CA │  ← Autoridad Certificadora
                    │  (la CA)     │
                    └──────┬───────┘
                           │
              firma certificados para todos
                           │
            ┌──────────────┼──────────────┐
            │              │              │
            ▼              ▼              ▼
     ┌─────────────┐ ┌─────────────┐ ┌─────────────┐
     │ server.crt  │ │ esp32_02.crt│ │ mqtt_client │
     │ (broker)    │ │ (ESP32)     │ │ .crt (Py)   │
     └─────────────┘ └─────────────┘ └─────────────┘
```

**Clave conceptual:** La CA firma cada certificado con su clave privada (`ca.key`). Cualquiera con el certificado público de la CA (`ca.crt`) puede verificar esa firma. Si la firma es válida, el certificado es legítimo.

### 4.2 - Cada certificado tiene un par de claves

```
Para el BROKER:
    server.crt  → certificado (contiene clave pública del broker)
    server.key  → clave privada del broker (SECRETO)

Para el ESP32:
    esp32_02.crt → certificado (contiene clave pública del ESP32)
    esp32_02.key → clave privada del ESP32 (SECRETO)
```

---

## PARTE 5: TLS paso a paso (handshake)

Ahora veamos exactamente qué pasa cuando el ESP32 se conecta al broker.

### Escenario: TLS normal (solo cliente verifica al servidor)

```
ESP32                                    Mosquitto Broker
  │                                              │
  │  1. "Hola, quiero conectarme al puerto 8883"│
  │──── ClientHello ────────────────────────────>│
  │     (propongo cifrar con AES-256)            │
  │                                              │
  │  2. "OK, usaremos AES-256. Aquí mi identidad"│
  │<──── ServerHello ───────────────────────────│
  │      + server.crt (certificado del broker)   │
  │                                              │
  │  3. ESP32 recibe server.crt                  │
  │     ¿Es válido? Verifica:                    │
  │     ✓ ¿Está firmado por ca.crt que tengo?   │
  │     ✓ ¿No está expirado?                     │
  │     ✓ ¿El CN dice "192.168.1.2" que esperaba?│
  │                                              │
  │  4. Intercambio de claves efímeras           │
  │     (ECDHE - permite Perfect Forward          │
  │      Secrecy: si mañana roban la clave       │
  │      privada del broker, las sesiones        │
  │      anteriores siguen seguras)              │
  │──── parámetros ECDHE ──────────────────────>│
  │<─── parámetros ECDHE ───────────────────────│
  │                                              │
  │  5. Ambos calculan una CLAVE DE SESIÓN       │
  │     (simétrica, usada para todo el tráfico)  │
  │                                              │
  │  6. Handshake completo                       │
  │<═════════ MQTT cifrado con AES-256 ════════>│
```

**Resultado:** El ESP32 **sabe** que habla con el broker legítimo. Pero el broker **no sabe** quién es el ESP32 (cualquiera podría haber completado el handshake).

### Escenario: mTLS (ambos se verifican mutuamente)

```
ESP32                                    Mosquitto Broker
  │                                              │
  │  1. ClientHello ────────────────────────────>│
  │                                              │
  │  2. ServerHello + server.crt ───────────────│
  │                                              │
  │  3. ESP32 verifica server.crt ✓              │
  │                                              │
  │  4. "Ahora te pido TU certificado"           │
  │<──── CertificateRequest ────────────────────│
  │                                              │
  │  5. ESP32 envía su certificado               │
  │──── esp32_02.crt ──────────────────────────>│
  │                                              │
  │  6. ESP32 prueba que tiene la clave privada  │
  │──── firma con esp32_02.key ────────────────>│
  │                                              │
  │  7. Broker recibe esp32_02.crt               │
  │     ¿Es válido? Verifica:                    │
  │     ✓ ¿Está firmado por ca.crt que tengo?   │
  │     ✓ ¿No está expirado?                     │
  │     ✓ ¿El CN dice "esp32_02" que esperaba?   │
  │     ✓ ¿La firma es auténtica?                │
  │                                              │
  │  8. Handshake completo                       │
  │<═════════ MQTT cifrado ════════════════════>│
```

**Resultado:** Ambos saben con quién hablan. El ESP32 confía en el broker, y el broker confía en el ESP32.

---

## PARTE 6: La criptografía detrás - cómo funciona la firma

Este es el punto más importante para entender:

### 6.1 - Firmar = "Probar que soy yo sin decir mi contraseña"

```
PARA FIRMAR (la CA firma un certificado):
    Mensaje: datos del certificado (CN, fecha, clave pública)
    Clave privada de la CA: ca.key
    → Firma digital

PARA VERIFICAR (el ESP32 verifica un certificado):
    Mensaje: los mismos datos
    Firma: la que viene en el certificado
    Clave pública de la CA: ca.crt
    → ¿Firma válida? Sí/No
```

**Proceso detallado de firma con SHA-256 + RSA:**

```
┌─────────────────────────────────────────────────────────────┐
│                    PROCESO DE FIRMA                          │
│                                                             │
│  1. Tomar el contenido del certificado                      │
│     Datos: {CN: "esp32_02", O: "EM-Devices", ...}          │
│                                                             │
│  2. Calcular hash con SHA-256                               │
│     SHA-256(Datos) → hash de 256 bits                       │
│     hash: "a1b2c3d4e5f6..."                                │
│                                                             │
│  3. Cifrar el hash con la clave privada de la CA            │
│     RSA(hash, ca.key) → firma                               │
│     firma: "xK#9f$2mNp..."                                 │
│                                                             │
│  4. El certificado firmado contiene:                        │
│     - Datos originales                                      │
│     - Firma                                                 │
│                                                             │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                 PROCESO DE VERIFICACIÓN                      │
│                                                             │
│  1. Extraer datos y firma del certificado                   │
│     Datos: {CN: "esp32_02", O: "EM-Devices", ...}          │
│     Firma: "xK#9f$2mNp..."                                 │
│                                                             │
│  2. Calcular hash de los datos con SHA-256                  │
│     SHA-256(Datos) → hash_calculado                         │
│                                                             │
│  3. Descifrar la firma con la clave pública de la CA        │
│     RSA_descifrar(firma, ca.crt) → hash_original           │
│                                                             │
│  4. Comparar hashes                                         │
│     hash_calculado == hash_original → Firma VÁLIDA ✓        │
│     hash_calculado != hash_original → Firma INVÁLIDA ✗     │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 - La prueba de posesión de clave privada

Cuando el ESP32 se conecta con mTLS, el broker le dice:

> "Prueba que eres esp32_02: cifra algo con tu clave privada"

El ESP32 toma un dato aleatorio del handshake, lo cifra con `esp32_02.key`, y lo envía. El broker lo descifra con `esp32_02.crt` (clave pública). Si coincide, el ESP32 tiene la clave privada → es legítimo.

```
Broker envía: "dato_aleatorio_12345"
ESP32 cifra:   RSA("dato_aleatorio_12345", esp32_02.key) → "xK#9f$2m"
Broker verifica: RSA_descifrar("xK#9f$2m", esp32_02.crt) → "dato_aleatorio_12345" ✓
```

---

## PARTE 7: ¿Por qué no solo usar username/password?

Podrías pensar: "¿Por qué no simplemente poner usuario y contraseña en el ESP32?"

```
ESP32 --"user:esp32_02 pass:1234"--> Broker
```

**Problema 1:** Sin TLS, el password viaja en texto plano. Si alguien escucha, lo tiene.

**Problema 2:** Aunque uses TLS, el password es estático. Si lo roban, puede reusarse.

**Problema 3:** No hay forma de revocar un password a distancia de forma criptográfica.

**mTLS resuelve estos problemas:**
- La clave privada nunca viaja por la red (solo se usa para firmar)
- Cada conexión usa claves de sesión efímeras
- Si un dispositivo se compromete, revocas su certificado en la CA y listo

---

## PARTE 8: Los comandos OpenSSL explicados

### 8.1 - Generar la CA (Autoridad Certificadora)

```bash
openssl genrsa -out ca.key 2048
```

| Parte | Significado |
|-------|-------------|
| `openssl` | La herramienta de criptografía más usada del mundo |
| `genrsa` | Generar una clave RSA (algoritmo de cifrado asimétrico) |
| `-out ca.key` | Guardar la clave privada en el archivo `ca.key` |
| `2048` | Tamaño de la clave en bits. 2048 = seguro. 4096 = más seguro pero más lento |

```bash
openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 \
  -out ca.crt -subj "/CN=EM-Root-CA/O=EM-Project"
```

| Parte | Significado |
|-------|-------------|
| `req` | Crear una solicitud de certificado (o en este caso, el certificado directamente) |
| `-x509` | "No me des un CSR, dame directamente el certificado firmado" |
| `-new` | Es un certificado nuevo |
| `-nodes` | "No encrypt the private key" - no cifrar la clave privada con contraseña (para que Mosquitto pueda leerla automáticamente) |
| `-key ca.key` | Usar esta clave privada para firmar |
| `-sha256` | Algoritmo de hash para la firma (SHA-256 es seguro; SHA-1 está obsoleto) |
| `-days 3650` | Válido por 3650 días = 10 años |
| `-out ca.crt` | Archivo de salida: el certificado de la CA |
| `-subj "/CN=EM-Root-CA/O=EM-Project"` | Datos del certificado: CN=Common Name (nombre), O=Organization |

### 8.2 - Generar el certificado del Broker

```bash
openssl genrsa -out server.key 2048
```
Genera la clave privada del broker. Misma lógica que la CA.

```bash
openssl req -new -key server.key -out server.csr \
  -subj "/CN=192.168.1.2/O=EM-MQTT-Broker"
```

| Parte | Significado |
|-------|-------------|
| `-new` | Certificado nuevo |
| `-key server.key` | Clave privada que irá emparejada con este certificado |
| `-out server.csr` | CSR = Certificate Signing Request (solicitud de firma) |
| `-subj "/CN=192.168.1.2"` | **CRÍTICO:** El CN debe coincidir con la IP/host que los clientes usan para conectarse al broker |

**¿Por qué el CN importa?** Porque el ESP32 verificará: "¿El certificado que me dio el broker dice que es `192.168.1.2`? Sí, eso es a donde me conecté." Si el CN no coincide, el ESP32 rechaza la conexión (protección contra MITM).

```bash
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key \
  -CAcreateserial -out server.crt -days 3650 -sha256
```

| Parte | Significado |
|-------|-------------|
| `x509` | Operación sobre certificados X.509 (el estándar de certificados) |
| `-req` | "Estoy tomando un CSR como entrada" |
| `-in server.csr` | El CSR que generamos antes |
| `-CA ca.crt` | Certificado de la CA (para incluirlo en la cadena) |
| `-CAkey ca.key` | Clave privada de la CA (para FIRMAR el certificado) |
| `-CAcreateserial` | Crear un archivo de serial (`ca.srl`) para rastrear certificados firmados |
| `-out server.crt` | Certificado firmado del broker |
| `-days 3650` | Válido 10 años |

### 8.3 - Generar el certificado del ESP32

```bash
openssl genrsa -out esp32_02.key 2048

openssl req -new -key esp32_02.key -out esp32_02.csr \
  -subj "/CN=esp32_02/O=EM-Devices"

openssl x509 -req -in esp32_02.csr -CA ca.crt -CAkey ca.key \
  -CAcreateserial -out esp32_02.crt -days 3650 -sha256
```

Misma lógica que el broker. **La diferencia clave:** el CN es `esp32_02` (el identificador del dispositivo). Mosquitto con `use_identity_as_username true` usará este CN como usuario MQTT automáticamente.

### 8.4 - Verificar la cadena de confianza

```bash
openssl verify -CAfile ca.crt server.crt
```

| Parte | Significado |
|-------|-------------|
| `verify` | Verificar la firma de un certificado |
| `-CAfile ca.crt` | Certificado de la CA de confianza |
| `server.crt` | El certificado a verificar |

Salida: `server.crt : OK` significa que `server.crt` fue firmado por `ca.crt`.

```bash
openssl x509 -in server.crt -text -noout
```

Muestra todos los campos del certificado: emisor, sujeto, fechas, clave pública, firma, etc.

---

## PARTE 9: Flujo completo en EM_server

### Diagrama de arquitectura de seguridad

```
┌──────────────────────────────────────────────────────────────────┐
│                    RED LOCAL (192.168.1.0/24)                    │
│                                                                  │
│  ┌─────────┐     TLS + mTLS     ┌──────────────┐                │
│  │  ESP32  │◄══════════════════►│  Mosquitto   │                │
│  │         │  puerto 8883       │  Broker      │                │
│  │ esp32_02│  cifrado AES-256   │  192.168.1.2 │                │
│  │ .crt    │  + certificados    │  server.crt  │                │
│  │ .key    │  mutuamente        │  server.key  │                │
│  └─────────┘  verificados       │  ca.crt      │                │
│                                 │  ca.key      │                │
│  ┌─────────┐     TLS            │  (en /etc/   │                │
│  │Python   │◄══════════════════►│   mosquitto/ │                │
│  │Server   │  puerto 8883       │   certs/)    │                │
│  │mqtt_    │  certificado de    │              │                │
│  │client   │  cliente propio    └──────────────┘                │
│  │.crt     │                                                     │
│  │.key     │                                                     │
│  └─────────┘                                                     │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### ¿Qué archivos van a cada dispositivo?

```
EM-Root-CA (ca.key + ca.crt)
    │
    ├──→ ca.crt va a TODOS (ESP32, Broker, Python Server)
    │    porque todos necesitan verificar certificados
    │
    └──→ ca.key se queda en un lugar SEGURO (USB offline)
         porque es el secreto más crítico. Si se roba,
         el atacante puede crear certificados falsos.

Broker Mosquitto (192.168.1.2)
    ├── ca.crt        (para verificar clientes)
    ├── server.crt    (su identidad)
    └── server.key    (su secreto)

ESP32 (esp32_02)
    ├── ca.crt        (para verificar al broker)
    ├── esp32_02.crt  (su identidad)
    └── esp32_02.key  (su secreto) → embebido en firmware como string C

Python Server
    ├── ca.crt        (para verificar al broker)
    ├── mqtt_client.crt (su identidad)
    └── mqtt_client.key (su secreto) → en /etc/em/certs/
```

---

## PARTE 10: ¿Qué pasa si algo falla?

### Escenario A: Certificado firmado por otra CA

```
ESP32 con ca.crt_A ──► Broker con cert firmado por ca.crt_B
                              │
    ESP32 verifica: ¿ca.crt_B está en mi confianza? NO
                              │
    Resultado: Conexión RECHAZADA
```

### Escenario B: Certificado expirado

```
ESP32 verifica: ¿fecha actual > fecha_expiración? SÍ
                │
    Resultado: Conexión RECHAZADA
```

### Escenario C: CN no coincide

```
ESP32 se conecta a IP 192.168.1.5
Broker entrega cert con CN=192.168.1.2
                │
ESP32 verifica: ¿CN=192.168.1.2 coincide con 192.168.1.5? NO
                │
    Resultado: Conexión RECHAZADA (posible ataque MITM)
```

### Escenario D: Alguien intenta suplantar al ESP32

```
ESP32 falso (sin certificado) ──► Broker
Broker pide: "Envía tu certificado"
ESP32 falso: "No tengo"
                │
    Resultado: Conexión RECHAZADA (require_certificate=true)
```

---

## PARTE 11: Resumen visual del proceso completo

```
FASE 1: Generar identidades (offline, una vez)
═══════════════════════════════════════════════

    openssl genrsa ──► ca.key
    openssl req -x509 ──► ca.crt

    openssl genrsa ──► server.key
    openssl req ──► server.csr
    openssl x509 (firmado por ca.key) ──► server.crt

    openssl genrsa ──► esp32_02.key
    openssl req ──► esp32_02.csr
    openssl x509 (firmado por ca.key) ──► esp32_02.crt


FASE 2: Distribuir (copiar archivos a cada dispositivo)
═══════════════════════════════════════════════════════

    ca.crt ──► Broker + ESP32 + Python
    server.crt + server.key ──► Broker
    esp32_02.crt + esp32_02.key ──► ESP32


FASE 3: Conexión (cada vez que el ESP32 se enciende)
═══════════════════════════════════════════════════════

    ESP32 ──► "Hola broker, quiero conectarme"
    Broker ──► "Aquí mi identidad (server.crt)"
    ESP32 ──► "¿Ese certificado es de confianza? SÍ ✓"
    Broker ──► "Ahora demuestra que eres esp32_02"
    ESP32 ──► "Aquí mi certificado (esp32_02.crt) + firma"
    Broker ──► "¿Ese certificado es de confianza? SÍ ✓"
    Broker ──► "Bienvenido, esp32_02"
    ════════ MQTT cifrado activo ════════
```

---

## PARTE 12: Glossario de términos

| Término | Definición |
|---------|-----------|
| **TLS** | Transport Layer Security - protocolo de cifrado para comunicaciones en red |
| **mTLS** | Mutual TLS - TLS donde ambos lados se autentican con certificados |
| **CA** | Certificate Authority - Autoridad Certificadora |
| **CSR** | Certificate Signing Request - Solicitud de firma de certificado |
| **PEM** | Privacy Enhanced Mail - formato de texto para certificados y claves |
| **RSA** | Algoritmo de cifrado asimétrico (Rivest-Shamir-Adleman) |
| **ECC** | Elliptic Curve Cryptography - cifrado asimétrico basado en curvas elípticas |
| **SHA-256** | Secure Hash Algorithm de 256 bits |
| **AES** | Advanced Encryption Standard - cifrado simétrico |
| **ECDHE** | Elliptic Curve Diffie-Hellman Ephemeral - intercambio de claves |
| **MITM** | Man-In-The-Middle - ataque de interposición |
| **X.509** | Estándar para certificados digitales |
| **HMAC** | Hash-based Message Authentication Code |
| **NTP** | Network Time Protocol - sincronización de reloj |
| **CN** | Common Name - nombre común del certificado |
| **SAN** | Subject Alternative Name - nombres alternativos del sujeto |

---

## PARTE 13: Referencias

- [RFC 8446 - TLS 1.3](https://tools.ietf.org/html/rfc8446)
- [RFC 5280 - X.509 PKI](https://tools.ietf.org/html/rfc5280)
- [OpenSSL Documentation](https://www.openssl.org/docs/)
- [Mosquitto TLS Configuration](https://mosquitto.org/man/mosquitto-conf-5.html)
- [ESP32 WiFiClientSecure](https://github.com/espressif/arduino-esp32/tree/master/libraries/WiFiClientSecure)
- [FIPS 180-4 - SHA-256](https://csrc.nist.gov/publications/detail/fips/180/4/final)
