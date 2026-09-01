/**
 * DEMO integrada — Wearable IoMT (ESP32-C3)
 * Acelerometro (MPU6050) + Temperatura IR (MLX90614) + OLED (SSD1306) + Wi-Fi.
 *
 * Pensado para mostrar: todos los subsistemas validados funcionando juntos.
 * NO incluye MAX30102 (Hito 5 sin empezar) ni MQTT (Hito 6b, necesita broker).
 *
 * COMUNICACION CON LA NOTEBOOK (misma red, sin broker):
 *   El ESP32 levanta un servidor HTTP en el puerto 80.
 *     http://<IP-del-ESP>/         -> pagina web con la telemetria en vivo
 *     http://<IP-del-ESP>/data     -> JSON (mismo formato que el contrato MQTT)
 *     http://wearable-pti.local/   -> lo mismo, via mDNS (si la notebook lo resuelve)
 *   La IP aparece en el OLED y por serie al conectar.
 *
 * Parametros clave (CLAUDE.md 7):
 *   - Umbral de impacto de caida: 2.8 g
 *   - Compensacion piel->central: +1.2 C  (PROVISIONAL, ver ESTADO.md)
 *   - Alerta de fiebre: > 38.0 C estimados (no 37.5: la medicion es gruesa)
 *
 * Bus I2C fijado a 100 kHz en todo momento (CLAUDE.md 3).
 * Nada de delay() bloqueante en loop() (CLAUDE.md 8).
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_MLX90614.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASS  (no versionado)

// ---------------- Constantes ----------------
static const int PIN_LED = 8;                 // onboard, activo en BAJO

static const int PIN_I2C_SDA = 5;
static const int PIN_I2C_SCL = 6;
static const uint32_t I2C_CLOCK_HZ = 100000;

static const uint8_t MPU6050_I2C_ADDR  = 0x68;
static const uint8_t MLX90614_I2C_ADDR = 0x5A;
static const uint8_t OLED_I2C_ADDR     = 0x3C;

static const uint8_t SCREEN_WIDTH  = 128;
static const uint8_t SCREEN_HEIGHT = 32;
static const int8_t  OLED_RESET_PIN = -1;

static const float    GRAVITY_MS2       = 9.80665f;
static const float    FALL_THRESHOLD_G  = 2.8f;    // CLAUDE.md 7
static const uint32_t FALL_LATCH_MS     = 5000;    // cuanto se muestra el evento
static const float    SKIN_TO_CORE_OFFSET_C = 1.2f; // PROVISIONAL
static const float    FEVER_THRESHOLD_C = 38.0f;   // indicador grueso

static const uint32_t IMU_PERIOD_MS  = 20;    // 50 Hz
static const uint32_t TEMP_PERIOD_MS = 500;   // 2 Hz
static const uint32_t OLED_PERIOD_MS = 500;
static const uint32_t WIFI_RETRY_MS  = 3000;

static const char* MDNS_HOST = "wearable-pti";

// ---------------- Objetos ----------------
Adafruit_MPU6050 mpu;
Adafruit_MLX90614 mlx = Adafruit_MLX90614();
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN,
                         I2C_CLOCK_HZ, I2C_CLOCK_HZ);
WebServer server(80);

// ---------------- Estado ----------------
struct Telemetry {
  float    accelG      = NAN;   // magnitud del vector aceleracion (g)
  float    tempSkinC   = NAN;   // MLX, superficie
  float    tempCoreEstC = NAN;  // + compensacion provisional
  float    ambientC    = NAN;   // Ta interno del MLX
  bool     fever       = false;
  const char* event    = "normal";   // "normal" | "fall"
  uint32_t fallAtMs    = 0;
};
Telemetry tel;

bool mpuOk = false, mlxOk = false, oledOk = false;
String deviceId = "wb-000000";

// ---------------- Sensores ----------------
static void sampleImu() {
  if (!mpuOk) return;
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);
  float gx = a.acceleration.x / GRAVITY_MS2;
  float gy = a.acceleration.y / GRAVITY_MS2;
  float gz = a.acceleration.z / GRAVITY_MS2;
  tel.accelG = sqrtf(gx * gx + gy * gy + gz * gz);

  if (tel.accelG > FALL_THRESHOLD_G) {
    tel.event = "fall";
    tel.fallAtMs = millis();
  } else if (tel.event[0] == 'f' && millis() - tel.fallAtMs > FALL_LATCH_MS) {
    tel.event = "normal";
  }
}

static void sampleTemp() {
  if (!mlxOk) return;
  float obj = mlx.readObjectTempC();
  float amb = mlx.readAmbientTempC();
  if (isnan(obj) || obj < -20.0f || obj > 380.0f) return;
  tel.tempSkinC    = obj;
  tel.ambientC     = amb;
  tel.tempCoreEstC = obj + SKIN_TO_CORE_OFFSET_C;
  tel.fever        = tel.tempCoreEstC > FEVER_THRESHOLD_C;
}

// ---------------- HTTP ----------------
static void buildJson(String& out) {
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["uptime_s"]  = millis() / 1000;
  doc["ip"]        = WiFi.localIP().toString();
  doc["rssi_dbm"]  = WiFi.RSSI();
  // Campos del contrato MQTT (CLAUDE.md 6). bpm/spo2 pendientes (Hito 5).
  doc["bpm"]       = nullptr;
  doc["spo2"]      = nullptr;
  if (!isnan(tel.tempCoreEstC)) doc["temp_c"] = round(tel.tempCoreEstC * 10) / 10.0;
  else                          doc["temp_c"] = nullptr;
  if (!isnan(tel.tempSkinC))    doc["temp_skin_c"] = round(tel.tempSkinC * 10) / 10.0;
  if (!isnan(tel.accelG))       doc["accel_g"] = round(tel.accelG * 100) / 100.0;
  doc["fever"]     = tel.fever;
  doc["event"]     = tel.event;
  doc["sensors"]["mpu6050"]  = mpuOk;
  doc["sensors"]["mlx90614"] = mlxOk;
  serializeJson(doc, out);
}

static void handleData() {
  String json;
  buildJson(json);
  server.send(200, "application/json", json);
}

static void handleRoot() {
  // Pagina autonoma: hace fetch('/data') cada segundo y actualiza el DOM.
  // Sin recursos externos (funciona sin internet).
  static const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wearable IoMT - PTI</title>
<style>
 body{font-family:system-ui,sans-serif;margin:0;background:#0f172a;color:#e2e8f0}
 header{padding:16px 20px;background:#1e293b;font-size:18px;font-weight:600}
 .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;padding:16px}
 .card{background:#1e293b;border-radius:10px;padding:16px}
 .k{font-size:12px;color:#94a3b8;text-transform:uppercase;letter-spacing:.5px}
 .v{font-size:28px;font-weight:700;margin-top:4px}
 .u{font-size:14px;color:#94a3b8}
 .alert{background:#7f1d1d}
 footer{padding:12px 20px;font-size:12px;color:#64748b}
 .dot{height:8px;width:8px;border-radius:50%;display:inline-block;margin-right:6px}
 .ok{background:#22c55e}.bad{background:#ef4444}
</style></head><body>
<header>Wearable IoMT &mdash; PTI <span id="dev" class="u"></span></header>
<div class="grid">
 <div class="card"><div class="k">Temp. central (est.)</div><div class="v"><span id="temp">--</span><span class="u"> &deg;C</span></div><div class="u" id="skin"></div></div>
 <div class="card"><div class="k">Aceleraci&oacute;n</div><div class="v"><span id="acc">--</span><span class="u"> g</span></div><div class="u">umbral caida 2.8 g</div></div>
 <div class="card"><div class="k">Evento</div><div class="v" id="evt">--</div></div>
 <div class="card"><div class="k">BPM / SpO2</div><div class="v u" style="font-size:18px">pendiente<br>(Hito 5)</div></div>
 <div class="card"><div class="k">Se&ntilde;al Wi-Fi</div><div class="v"><span id="rssi">--</span><span class="u"> dBm</span></div></div>
 <div class="card"><div class="k">Uptime</div><div class="v"><span id="up">--</span><span class="u"> s</span></div></div>
</div>
<footer>
 <span class="dot" id="d_mpu"></span>MPU6050
 &nbsp;<span class="dot" id="d_mlx"></span>MLX90614
 &nbsp;|&nbsp; IP <span id="ip"></span>
 &nbsp;|&nbsp; actualiza cada 1 s
</footer>
<script>
async function tick(){
 try{
  const r=await fetch('/data'); const d=await r.json();
  dev.textContent=d.device_id;
  temp.textContent=d.temp_c??'--';
  skin.textContent=d.temp_skin_c!=null?('piel '+d.temp_skin_c+' °C'):'';
  acc.textContent=d.accel_g??'--';
  evt.textContent=d.event; evt.parentElement.className='card'+(d.event=='fall'||d.fever?' alert':'');
  rssi.textContent=d.rssi_dbm; up.textContent=d.uptime_s; ip.textContent=d.ip;
  d_mpu.className='dot '+(d.sensors.mpu6050?'ok':'bad');
  d_mlx.className='dot '+(d.sensors.mlx90614?'ok':'bad');
 }catch(e){}
}
setInterval(tick,1000); tick();
</script>
</body></html>
)HTML";
  server.send_P(200, "text/html", PAGE);
}

// ---------------- OLED ----------------
static void renderOled() {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  if (WiFi.status() == WL_CONNECTED) display.print(WiFi.localIP().toString());
  else                              display.print(F("WiFi..."));

  display.setCursor(0, 11);
  if (!isnan(tel.tempCoreEstC)) display.printf("T %.1fC%s", tel.tempCoreEstC, tel.fever ? " FIEBRE" : "");
  else                         display.print(F("T --"));

  display.setCursor(0, 22);
  if (!isnan(tel.accelG)) display.printf("a %.2fg  %s", tel.accelG, tel.event);
  else                    display.print(F("a --"));

  display.display();
}

// ---------------- Wi-Fi ----------------
static void wifiConnect() {
  WiFi.disconnect(true, true);
  delay(50);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WiFi: conectando a \"%s\"...\n", WIFI_SSID);
}

// ---------------- setup / loop ----------------
void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  Serial.println();
  Serial.println(F("=== DEMO integrada - Wearable IoMT ==="));

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(I2C_CLOCK_HZ);

  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR);
  mpuOk  = mpu.begin(MPU6050_I2C_ADDR, &Wire);
  mlxOk  = mlx.begin(MLX90614_I2C_ADDR, &Wire);
  Serial.printf(" OLED:%s  MPU6050:%s  MLX90614:%s\n",
                oledOk ? "OK" : "--", mpuOk ? "OK" : "--", mlxOk ? "OK" : "--");

  if (mpuOk) {
    mpu.setAccelerometerRange(MPU6050_RANGE_16_G);   // margen para picos de impacto
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  }

  // Device ID estable = "wb-" + ultimos 3 bytes de la MAC.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[16];
  snprintf(buf, sizeof(buf), "wb-%02x%02x%02x", mac[3], mac[4], mac[5]);
  deviceId = buf;
  Serial.printf(" device_id: %s\n", deviceId.c_str());

  wifiConnect();

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  Serial.println(F(" HTTP server en puerto 80"));
}

void loop() {
  static uint32_t lastImu = 0, lastTemp = 0, lastOled = 0, lastWifi = 0;
  static bool wasConnected = false, mdnsUp = false;
  uint32_t now = millis();

  // --- Wi-Fi ---
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(PIN_LED, LOW);
    if (!wasConnected) {
      wasConnected = true;
      Serial.printf("WiFi OK  IP: %s  RSSI: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      Serial.printf("  -> http://%s/   |   http://%s.local/\n",
                    WiFi.localIP().toString().c_str(), MDNS_HOST);
      if (!mdnsUp && MDNS.begin(MDNS_HOST)) {
        MDNS.addService("http", "tcp", 80);
        mdnsUp = true;
      }
    }
  } else {
    digitalWrite(PIN_LED, (now / 200) % 2 ? HIGH : LOW);
    if (wasConnected) { wasConnected = false; Serial.println(F("WiFi: caido")); }
    if (now - lastWifi >= WIFI_RETRY_MS) { lastWifi = now; wifiConnect(); }
  }

  // --- HTTP ---
  server.handleClient();

  // --- Sensores ---
  if (now - lastImu  >= IMU_PERIOD_MS)  { lastImu  = now; sampleImu(); }
  if (now - lastTemp >= TEMP_PERIOD_MS) { lastTemp = now; sampleTemp(); }

  // --- OLED ---
  if (now - lastOled >= OLED_PERIOD_MS) { lastOled = now; renderOled(); }
}
