/**
 * Hito de validacion #6a — Asociacion Wi-Fi
 * (CLAUDE.md seccion 9, hito 6, parte Wi-Fi. La parte MQTT es el 6b.)
 *
 * Objetivo: confirmar que el ESP32-C3 asocia al AP, obtiene IP y tiene señal
 * suficiente. NO toca sensores ni broker: es un test aislado.
 *
 * Al arrancar hace un scan y lista los AP visibles (util para verificar que
 * tu red aparece y esta en 2.4 GHz: el C3 no tiene 5 GHz). Despues conecta,
 * imprime IP / gateway / DNS / RSSI / canal / MAC y deriva el Client ID MQTT
 * de la MAC (estable entre reinicios, como pide CLAUDE.md 6), para el 6b.
 *
 * Reconexion no bloqueante en loop() (CLAUDE.md 8: nada de delay() en loop).
 * Bus I2C del OLED fijado a 100 kHz (CLAUDE.md 3).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASS  (no versionado)

// --- LED onboard: GPIO 8, activo en BAJO ---
static const int PIN_LED = 8;

// --- OLED 128x32 en el bus I2C ---
static const int PIN_I2C_SDA = 5;
static const int PIN_I2C_SCL = 6;
static const uint32_t I2C_CLOCK_HZ = 100000;
static const uint8_t SCREEN_WIDTH  = 128;
static const uint8_t SCREEN_HEIGHT = 32;
static const uint8_t OLED_I2C_ADDR = 0x3C;
static const int8_t  OLED_RESET_PIN = -1;

// --- Parametros de conexion ---
// El primer WiFi.begin() tras el boot suele quedarse en IDLE y fallar; el
// segundo intento conecta (quirk conocido del ESP32-C3). Timeout corto para
// que el retry util llegue rapido.
static const uint32_t CONNECT_TIMEOUT_MS = 12000;   // por intento
static const uint32_t RETRY_PAUSE_MS     = 1000;    // entre intentos
static const uint32_t STATUS_PERIOD_MS   = 3000;    // refresco de RSSI/estado

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN,
                         I2C_CLOCK_HZ, I2C_CLOCK_HZ);
bool oledPresent = false;

String mqttClientId;   // "wb-XXXXXX" derivado de la MAC

// Texto legible para wl_status_t.
static const char* wlStatusStr(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:    return "IDLE";
    case WL_NO_SSID_AVAIL:  return "NO_SSID_AVAIL (SSID no encontrado / 5GHz / oculto)";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED:      return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED (clave incorrecta?)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:   return "DISCONNECTED";
    default:                return "??";
  }
}

// Calidad aproximada a partir del RSSI (dBm).
static const char* rssiQuality(int rssi) {
  if (rssi >= -60) return "excelente";
  if (rssi >= -70) return "buena";
  if (rssi >= -80) return "justa";
  return "mala (antena PCB de la SuperMini es pobre - acercar al AP)";
}

static void oledLines(const String& l1, const String& l2, const String& l3) {
  if (!oledPresent) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);  display.print(l1);
  display.setCursor(0, 11); display.print(l2);
  display.setCursor(0, 22); display.print(l3);
  display.display();
}

static void scanNetworks() {
  Serial.println(F("Escaneando APs visibles..."));
  int n = WiFi.scanNetworks(false, true);   // sync, incluye ocultos
  if (n <= 0) {
    Serial.println(F("  (ninguno) - antena, o no hay 2.4 GHz cerca"));
    return;
  }
  Serial.printf("  %d encontrados:\n", n);
  Serial.println(F("   RSSI  ch  enc  SSID"));
  for (int i = 0; i < n; i++) {
    bool isTarget = (WiFi.SSID(i) == WIFI_SSID);
    Serial.printf("  %4d  %2d   %s  %s%s\n",
                  WiFi.RSSI(i), WiFi.channel(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPN" : "SEC",
                  WiFi.SSID(i).length() ? WiFi.SSID(i).c_str() : "<oculto>",
                  isTarget ? "   <== objetivo" : "");
  }
  WiFi.scanDelete();
}

static void printConnectionInfo() {
  Serial.println();
  Serial.println(F("=== CONECTADO ==="));
  Serial.printf("  SSID     : %s\n", WiFi.SSID().c_str());
  Serial.printf("  BSSID    : %s  (canal %d)\n", WiFi.BSSIDstr().c_str(), WiFi.channel());
  Serial.printf("  IP       : %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("  Gateway  : %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("  DNS      : %s\n", WiFi.dnsIP().toString().c_str());
  Serial.printf("  Mascara  : %s\n", WiFi.subnetMask().toString().c_str());
  Serial.printf("  MAC      : %s\n", WiFi.macAddress().c_str());
  int rssi = WiFi.RSSI();
  Serial.printf("  RSSI     : %d dBm  (%s)\n", rssi, rssiQuality(rssi));
  Serial.printf("  ClientID : %s   (para MQTT, Hito 6b)\n", mqttClientId.c_str());
  Serial.println();
}

// Intenta conectar una vez. Devuelve true si asocio dentro del timeout.
static bool connectOnce() {
  Serial.printf("Conectando a \"%s\" ...\n", WIFI_SSID);
  oledLines("WiFi: conectando", String(WIFI_SSID), "");

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);            // sin power save durante el test
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  wl_status_t last = WL_IDLE_STATUS;
  while (millis() - start < CONNECT_TIMEOUT_MS) {
    wl_status_t st = WiFi.status();
    if (st != last) {
      Serial.printf("  [%5lu ms] %s\n", (unsigned long)(millis() - start), wlStatusStr(st));
      last = st;
    }
    if (st == WL_CONNECTED) return true;
    // Parpadeo mientras intenta.
    digitalWrite(PIN_LED, (millis() / 200) % 2 ? HIGH : LOW);
    delay(20);
  }
  Serial.printf("  timeout tras %lu ms (ultimo estado: %s)\n",
                (unsigned long)CONNECT_TIMEOUT_MS, wlStatusStr(WiFi.status()));
  return false;
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && (millis() - start) < 3000) delay(10);

  Serial.println();
  Serial.println(F("=== Hito #6a - Asociacion Wi-Fi (ESP32-C3, solo 2.4 GHz) ==="));

  if (strlen(WIFI_SSID) == 0) {
    Serial.println(F("[ERROR] WIFI_SSID vacio. Completar src/secrets.h"));
  }

  // OLED (opcional).
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(I2C_CLOCK_HZ);
  oledPresent = display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR);
  Serial.printf(" OLED: %s\n", oledPresent ? "OK" : "ausente");

  WiFi.mode(WIFI_STA);

  // Client ID MQTT estable = "wb-" + ultimos 3 bytes de la MAC.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[16];
  snprintf(buf, sizeof(buf), "wb-%02x%02x%02x", mac[3], mac[4], mac[5]);
  mqttClientId = buf;

  scanNetworks();
}

void loop() {
  static bool wasConnected = false;
  static uint32_t lastStatus = 0;
  static uint32_t lastRetry = 0;

  uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(PIN_LED, LOW);   // LED fijo = conectado

    if (!wasConnected) {
      wasConnected = true;
      printConnectionInfo();
    }

    if (now - lastStatus >= STATUS_PERIOD_MS) {
      lastStatus = now;
      int rssi = WiFi.RSSI();
      Serial.printf("[%lus] conectado  IP %s  RSSI %d dBm (%s)\n",
                    (unsigned long)(now / 1000),
                    WiFi.localIP().toString().c_str(), rssi, rssiQuality(rssi));
      oledLines("WiFi OK  " + String(rssi) + "dBm",
                WiFi.localIP().toString(),
                mqttClientId);
    }
    return;
  }

  // No conectado.
  if (wasConnected) {
    wasConnected = false;
    Serial.println(F("[!] Conexion perdida. Reintentando..."));
    oledLines("WiFi: caido", "reintentando...", "");
  }

  if (now - lastRetry >= RETRY_PAUSE_MS || lastRetry == 0) {
    lastRetry = now;
    if (connectOnce()) {
      // El bloque de arriba imprime la info en la proxima vuelta.
    } else {
      Serial.printf("Nuevo intento en %lu s...\n\n", (unsigned long)(RETRY_PAUSE_MS / 1000));
      oledLines("WiFi: sin conexion", "reintento en 5s", "");
    }
  }
}
