# Estado del proyecto — Firmware wearable IoMT (POC PTI)

> Handoff para continuar en otra máquina. Fecha: 2026-09-01.
> La guía de desarrollo completa está en `CLAUDE.md` (copia sincronizada de la de `Desktop/PTI`).

---

## Dónde estamos

Validación de hardware por hitos (ver `CLAUDE.md` sección 9). Se avanza en orden, sin
pasar al siguiente hasta que el anterior pasa.

| Hito | Estado | Notas |
| :--- | :--- | :--- |
| 1. Escáner I²C | ✅ **PASA** | Las 4 direcciones detectadas: `0x3C` OLED, `0x57` MAX30102, `0x5A` MLX90614, `0x68` MPU6050 |
| 2. OLED SSD1306 | ✅ **PASA** | Texto estático OK. Ver gotcha del reloj abajo. |
| 3. MPU6050 | ✅ **PASA** | ~1.05 g en reposo (dentro de tolerancia, offset de fábrica). REST/MOV OK. |
| 4. MLX90614 | ⚠️ **PARCIAL — recalibración fina pendiente** | El sensor funciona tras recuperar la EEPROM. Falta la curva de compensación fina, que requiere la carcasa/correa (ver abajo). |
| 5. MAX30102 (BPM/SpO2) | ⛔ **NO EMPEZADO** | Próximo paso. |
| 6a. Wi-Fi (asociación) | ✅ **PASA** | Asocia, obtiene IP, RSSI excelente. Quirk: el 1er `WiFi.begin()` post-boot falla, el 2do conecta (retry automático). Client ID MQTT = `wb-24d7cc` (derivado de la MAC `44:BD:8D:24:D7:CC`). |
| 6b. MQTT | ⛔ pendiente | Depende de tener Mosquitto corriendo en el Edge Gateway. `MQTT_BROKER` en `secrets.h` hay que ponerlo con la IP real del gateway. |
| 7. Cifrado AES-256 | ⛔ pendiente | |
| 8. Detección de caídas | ⛔ pendiente | |

---

## Hito 4 — MLX90614: qué pasó y qué falta

### Lo resuelto

El sensor medía **+28 °C sobre piel** (61 °C en muñeca real ~33 °C), estable, con
emisividad 1.00 y `Ta` correcta. Causa: **EEPROM `Config1` (registro `0x25`) corrupta**
— ganancia en ×12.5 en vez de ×100 y bits de signo Ks/KT2 invertidos (leído `0xAFF4`,
correcto `0x9FB4`). Probable origen: ESD por punta de soldador sin descarga a tierra
al soldar los pines (era la primera soldadura).

**Recuperado** reescribiendo `0x25 = 0x9FB4` (ciclo borrar→escribir SMBus) y **ciclando
la alimentación del sensor** (el MLX solo carga la config de EEPROM al arrancar; el
reset del ESP32 no alcanza si el sensor sigue con 3V3). El sketch de diagnóstico
profundo con esa capacidad quedó en el historial de git (commit inicial); la lógica
está en las funciones `rawWrite16` / `eepromWriteCell` / `writeConfig1Flow`.

### Verificación post-fix

| Objetivo | Real | MLX | Error | Comentario |
| :--- | :--- | :--- | :--- | :--- |
| Aire | = Ta | = Ta | ~0 | OK |
| Agua tibia | 35.7 | ~40.5 | +4.8 | **Artefacto: condensación del vapor sobre el lente. Descartar.** |
| Muñeca (seco, asentado) | 36.3–36.8 (sublingual) | 35.3–36.2 | −0.5 a −1.4 | Lectura buena; delta piel→central razonable |

Repetibilidad de 3 mediciones seguidas de la misma muñeca: MLX entre **35.3 y 37.5**
(~1–2 °C de dispersión). La prueba más limpia y asentada dio piel ≈ central − 1.2 °C
a ambiente ~23 °C.

### Qué falta (recalibración fina)

1. La dispersión de ~1–2 °C está dominada por la **geometría no repetible** (sensor
   sostenido a mano; gap/ángulo/sellado de la cavidad de aire cambian entre medidas).
   **La curva `central ≈ f(piel, Ta)` solo tiene sentido con la correa/carcasa que
   fije la geometría.** Calibrar antes = caracterizar el ruido del pulso.
2. Cuando exista el fixture físico: recolectar 5–8 puntos `(piel_avg30, Ta, comercial)`
   a distintos ambientes (sala normal, frente a ventilador/AC, ambiente < 20 °C, otra
   persona) y ajustar `central ≈ a·piel + b·Ta + c`. Llevar el resultado a `config.h`.
3. Provisional hasta entonces: `SKIN_TO_CORE_OFFSET_C = 1.2f`.
4. Firmware: temperatura como **indicador grueso**. Alerta de fiebre en **> 38.0 °C**
   estimados (no 37.5) para no acumular falsos positivos.
5. El sensor cerrado contra la muñeca se **autocalienta** (`Ta` +3.6 °C en 3 min
   continuos). La carcasa debe disipar o el firmware compensar `Ta` según uptime.
6. Conseguir un **GY-906 de repuesto** (el actual tiene historial de daño; alimentarlo
   siempre a 3.3 V, soldar con soldador con tierra o muñequera antiestática).

---

## Próximo trabajo (en orden)

1. **Recalibración fina de temperatura** — bloqueada hasta tener la carcasa/correa.
   Mientras tanto usar el offset provisional.
2. **Hito 5 — MAX30102 (BPM / SpO2).** El más delicado de calibrar. Empezar con
   lectura de BPM estable con el dedo apoyado y SpO2 contra un oxímetro comercial.
   Librería: `sparkfun/SparkFun MAX3010x`. Dirección `0x57`. Ojo con la alimentación:
   el módulo violeta necesita **5 V en VIN** (ver `CLAUDE.md` 2.2).
3. **Hito 6b — MQTT.** El sketch de Wi-Fi (`src/main.cpp` actual) ya deja la base:
   asociación + Client ID. Falta: PubSubClient contra Mosquitto, `client.setBufferSize(512)`
   (gotcha CLAUDE.md 3), publicar JSON en claro al tópico `hospital/{sala}/wearable/{id}/data`
   y verificar con `mosquitto_sub`. Necesita el gateway corriendo.
4. Hitos 7–8 después.

Cuando el firmware pase de POCs a estructura real: wrappers de sensores con interfaz
común (`begin()`/`read()`/`isHealthy()`), constantes a `config.h`, módulos
`fall_detection/` `crypto/` `net/` `ui/` en `lib/` (ver `CLAUDE.md` 5 y 8).

---

## Gotchas descubiertos esta sesión (ya en CLAUDE.md sección 3)

- **Adafruit_SSD1306 sube el bus a 400 kHz** durante cada `display.display()` y lo baja
  después. En bus compartido con el MLX90614 (SMBus, máx 100 kHz) + 4 dispositivos
  hand-wired eso corrompe la transferencia (píxeles al azar). Fijar el constructor:
  `Adafruit_SSD1306 display(W, H, &Wire, RST, 100000, 100000);`
- **MLX90614 Config1 corrupta** — síntoma, causa y fix descritos arriba y en CLAUDE.md.
- **MLX90614 siempre a 3.3 V, nunca 5 V.**
- **`pio` no está en el PATH** de la terminal en la máquina original; se usaba
  `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"`. En la notebook puede variar.

---

## Cómo compilar / subir

```bash
pio run                       # compilar
pio run -t upload             # subir por USB-C
pio run -t upload -t monitor  # subir + monitor serie (115200)
pio device monitor            # solo monitor
```

Si el puerto está ocupado: cerrar el monitor serie antes de subir. Si la placa no
aparece: mantener BOOT presionado mientras se conecta el USB.

El sketch actual en `src/main.cpp` es el de **calibración del Hito 4** (arranque
escalonado: 5 s patrón OLED + 5 s cuenta regresiva + activación MLX, luego stream con
promedio móvil de 30 s y detección de asentamiento).
