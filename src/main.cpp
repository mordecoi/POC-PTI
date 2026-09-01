/**
 * Hito de validacion #4 — MLX90614 / GY-906 (temperatura IR)
 * Calibracion piel -> central con analisis de asentamiento termico.
 *
 * El MLX90614 mide temperatura SUPERFICIAL (muneca: 31-34 C), no central.
 * Para comparar contra el umbral de fiebre (37.5 C central) hace falta una
 * curva de compensacion. Este sketch recolecta los pares para armarla.
 *
 * NOTA: este sensor tuvo la EEPROM Config1 corrupta y se recupero reescribiendo
 * 0x25 = 0x9FB4 (ver CLAUDE.md 3). Si vuelve a leer con error de escala,
 * revisar ese registro con el sketch de diagnostico profundo.
 *
 * Metodo de medicion:
 *   - Sujetar el sensor con algo (NO la mano) a distancia FIJA de la muneca
 *     (2-5 mm), sin tocar la piel.
 *   - Esperar a 'ASENTADO' (la deriva de 10 s cae debajo de 0.1 C).
 *   - Anotar (obj_avg30, Ta, referencia_comercial) en una tabla.
 *   - Repetir a distintas temperaturas de ambiente y de piel.
 *
 * Columnas:
 *   obj_raw    : temp de objeto instantanea (piel)
 *   obj_avg30  : promedio movil de 30 s
 *   deriva/10s : cambio de obj_avg30 en los ultimos 10 s -> tiende a 0 al asentar
 *   Ta_now     : temperatura interna del sensor (ambiente)
 *   Ta_drift   : Ta_now - Ta al activar -> si crece, el sensor se autocalienta
 *
 * Arranque escalonado: 5 s patron OLED + 5 s cuenta regresiva + activacion MLX.
 * Bus fijado a 100 kHz en TODO momento (incluido el burst del SSD1306).
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MLX90614.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- Bus I2C (CLAUDE.md 2.1 / 2.3) ---
static const int PIN_I2C_SDA = 5;
static const int PIN_I2C_SCL = 6;
static const uint32_t I2C_CLOCK_HZ = 100000;

// --- LED onboard: GPIO 8, activo en BAJO ---
static const int PIN_LED = 8;

// --- Direcciones I2C (CLAUDE.md 2) ---
static const uint8_t MLX90614_I2C_ADDR = 0x5A;
static const uint8_t OLED_I2C_ADDR     = 0x3C;

// --- OLED 128x32 ---
static const uint8_t SCREEN_WIDTH  = 128;
static const uint8_t SCREEN_HEIGHT = 32;
static const int8_t  OLED_RESET_PIN = -1;

// --- Arranque escalonado ---
static const uint32_t PATTERN_MS   = 5000;
static const uint32_t COUNTDOWN_MS = 5000;

// --- Muestreo y ventanas ---
static const uint32_t SAMPLE_PERIOD_MS = 500;    // 2 Hz
static const uint8_t  AVG_WINDOW       = 60;     // 30 s de promedio movil
static const uint8_t  SLOPE_LAG        = 20;     // 10 s atras para la deriva
static const float    FEVER_THRESHOLD_C = 37.5f; // CLAUDE.md 7 (central)
static const float    SETTLED_SLOPE_C   = 0.10f; // |deriva/10s| para "asentado"

// Compensacion piel -> central. A medir en este hito; luego va a config.h.
// Primer par observado: piel ~34.0 C, Ta ~23 C, central (sublingual) 36.8 C
// -> delta ~ +2.8 C a ambiente ~23 C. Falta confirmar con mas puntos.
static const float SKIN_TO_CORE_OFFSET_C = 0.0f;

Adafruit_MLX90614 mlx = Adafruit_MLX90614();

// Bus a 100 kHz siempre (CLAUDE.md 3): por defecto la libreria sube a 400 kHz
// durante cada display.display() y corrompe el bus compartido con el MLX.
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN,
                         I2C_CLOCK_HZ, I2C_CLOCK_HZ);

bool oledPresent = false;
bool mlxActive   = false;

float objRing[AVG_WINDOW];
uint8_t objCount = 0, objHead = 0;

float avgRing[SLOPE_LAG];
uint8_t avgCount = 0, avgHead = 0;

float taAtActivation = NAN;
uint32_t activationMs = 0;

static void haltWithError(const char* msg) {
  Serial.printf("[ERROR] %s\n", msg);
  Serial.println(F("Revisar: 0x5A en el escaner? VCC del GY-906 en 3V3? Bus a 100 kHz?"));
  pinMode(PIN_LED, OUTPUT);
  for (;;) {
    digitalWrite(PIN_LED, LOW); delay(120);
    digitalWrite(PIN_LED, HIGH); delay(120);
  }
}

static void drawTestPattern() {
  if (!oledPresent) return;
  display.clearDisplay();
  display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  display.drawLine(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, SSD1306_WHITE);
  display.drawLine(0, SCREEN_HEIGHT - 1, SCREEN_WIDTH - 1, 0, SSD1306_WHITE);
  for (int x = 16; x < SCREEN_WIDTH; x += 16) display.drawLine(x, 0, x, SCREEN_HEIGHT - 1, SSD1306_WHITE);
  for (int y = 16; y < SCREEN_HEIGHT; y += 16) display.drawLine(0, y, SCREEN_WIDTH - 1, y, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(3, 12);
  display.print(F(" OLED OK "));
  display.display();
}

static void drawCountdown(uint32_t secsLeft) {
  if (!oledPresent) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("OLED solo en el bus"));
  display.setCursor(0, 12);
  display.printf("Activo MLX en %lus", (unsigned long)secsLeft);
  display.display();
}

static float pushObjGetAvg(float value) {
  objRing[objHead] = value;
  objHead = (objHead + 1) % AVG_WINDOW;
  if (objCount < AVG_WINDOW) objCount++;
  float sum = 0.0f;
  for (uint8_t i = 0; i < objCount; i++) sum += objRing[i];
  return sum / objCount;
}

static float pushAvgGetSlope(float avg) {
  float old = NAN;
  if (avgCount == SLOPE_LAG) old = avgRing[avgHead];
  avgRing[avgHead] = avg;
  avgHead = (avgHead + 1) % SLOPE_LAG;
  if (avgCount < SLOPE_LAG) avgCount++;
  if (isnan(old)) return NAN;
  return avg - old;
}

static void renderOled(float objAvg, float taNow, float slope, bool settled) {
  if (!oledPresent) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("piel30 %.2fC", objAvg);
  display.setCursor(0, 11);
  display.printf("Ta %.1f d%+.2f", taNow, isnan(slope) ? 0.0f : slope);
  display.setCursor(0, 22);
  display.print(settled ? F("ASENTADO - anotar") : F("asentando..."));
  display.display();
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && (millis() - start) < 10000) {
    digitalWrite(PIN_LED, (millis() / 250) % 2 ? HIGH : LOW);
    delay(10);
  }
  digitalWrite(PIN_LED, HIGH);

  Serial.println();
  Serial.println(F("=== Hito #4 - MLX90614 calibracion piel->central ==="));
  Serial.printf(" SDA=GPIO%d  SCL=GPIO%d  clock=%lu Hz (fijo)\n",
                PIN_I2C_SDA, PIN_I2C_SCL, (unsigned long)I2C_CLOCK_HZ);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(I2C_CLOCK_HZ);

  oledPresent = display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR);
  Serial.printf(" OLED: %s\n", oledPresent ? "OK" : "ausente (sigo por serie)");

  Serial.println(F(" [0 s] Patron de prueba (5 s)."));
  drawTestPattern();
  delay(PATTERN_MS);

  Serial.println(F(" [5 s] OLED solo en el bus. Activo el MLX en 5 s..."));
  for (int s = COUNTDOWN_MS / 1000; s > 0; s--) {
    drawCountdown(s);
    Serial.printf("   ... %d\n", s);
    delay(1000);
  }

  Serial.println(F(" [10 s] Activando MLX90614..."));
  if (!mlx.begin(MLX90614_I2C_ADDR, &Wire)) {
    haltWithError("mlx.begin() fallo (el MLX90614 no respondio en 0x5A)");
  }
  mlxActive = true;
  activationMs = millis();
  taAtActivation = mlx.readAmbientTempC();

  Serial.printf("   Emisividad: %.2f   Ta inicial: %.2f C\n",
                mlx.readEmissivity(), taAtActivation);
  Serial.println(F("   Sensor a distancia FIJA de la muneca (sujeto, no con la mano)."));
  Serial.println(F("   Al llegar a 'ASENTADO' anotar: obj_avg30 | Ta_now | termometro comercial"));
  Serial.println();
  Serial.println(F("  t(s)  obj_raw  obj_avg30  deriva/10s  Ta_now  Ta_drift  central_est  estado"));
  digitalWrite(PIN_LED, LOW);
}

void loop() {
  if (!mlxActive) return;

  static uint32_t lastSample = 0;
  uint32_t now = millis();
  if (now - lastSample < SAMPLE_PERIOD_MS) return;
  lastSample = now;

  float objC = mlx.readObjectTempC();
  float taC  = mlx.readAmbientTempC();

  if (isnan(objC) || objC < -20.0f || objC > 380.0f) {
    Serial.println(F("  lectura invalida (NaN / fuera de rango) - revisar cableado del GY-906"));
    return;
  }

  float objAvg  = pushObjGetAvg(objC);
  float slope   = pushAvgGetSlope(objAvg);
  float taDrift = taC - taAtActivation;
  float coreEst = objAvg + SKIN_TO_CORE_OFFSET_C;
  uint32_t tSec = (now - activationMs) / 1000;

  bool settled = !isnan(slope) && fabsf(slope) <= SETTLED_SLOPE_C;

  const char* estado;
  if (!settled) {
    estado = "asentando";
  } else if (fabsf(taDrift) > 1.0f) {
    estado = "ASENTADO (ojo: Ta derivo >1C, autocalentamiento)";
  } else {
    estado = "ASENTADO - anotar obj_avg30 / Ta / comercial";
  }

  Serial.printf("%6lu  %7.2f  %9.2f  %10s  %6.2f  %+8.2f  %11.2f  %s\n",
                (unsigned long)tSec, objC, objAvg,
                isnan(slope) ? "   --   " : String(slope, 2).c_str(),
                taC, taDrift, coreEst, estado);

  renderOled(objAvg, taC, slope, settled);
}
