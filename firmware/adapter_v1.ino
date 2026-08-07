/*******************************************************************************
 * Sensors:
 *   - ENS210      temperature + humidity   (I2C 0x43)
 *   - BMP580      pressure + temperature    (I2C 0x47, default)
 *   - RX8900      real-time clock           (I2C 0x32, default)
 *   - LIS2DW12    3-axis accelerometer      (SPI, CS on IO18)
 *   - SCD41       CO2 / T / RH              (I2C 0x62)
 *                 pressure comp. from BMP580, temperature offset ref from ENS210
 *   - 4x HT16K33A 8x8 bicolour matrices     (I2C 0x70..0x73, drawn as one 32x8)
 *
 * Display modes (cycled with S2):
 *   0 = clock
 *   1 = fire
 *   2 = particles
 *
 ******************************************************************************/

#include <Wire.h>
#include <SPI.h>

#include <Adafruit_GFX.h>
#include "Adafruit_LEDBackpack.h"

#include <SparkFun_BMP581_Arduino_Library.h>
#include "SparkFun_SCD4x_Arduino_Library.h"
#include "ens210.h"
#include <TimeLib.h>
#include <RX8900RTC.h>
#include <LIS2DW12Sensor.h>

using namespace ScioSense;

// ---------------------------------------------------------------------------
// Pin definitions
// ---------------------------------------------------------------------------
static const int PIN_SDA     = 3;
static const int PIN_SCL     = 19;
static const int PIN_SCK     = 5;
static const int PIN_MISO    = 6;
static const int PIN_MOSI    = 7;
static const int PIN_CS_ACC  = 18;
static const int PIN_INT1    = 4;   // LIS2DW12 interrupt (unused for now)
static const int PIN_BTN_S1  = 0;   // brightness
static const int PIN_BTN_S2  = 1;   // mode

// ---------------------------------------------------------------------------
// I2C addresses
// ---------------------------------------------------------------------------
static const uint8_t MATRIX_ADDR[4] = { 0x70, 0x71, 0x72, 0x73 };

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------
Adafruit_BicolorMatrix matrix[4] = {
  Adafruit_BicolorMatrix(), Adafruit_BicolorMatrix(),
  Adafruit_BicolorMatrix(), Adafruit_BicolorMatrix()
};

BMP581           bmp;            // BMP580/BMP581
SCD4x            scd(SCD4x_SENSOR_SCD41);
ENS210           ens210;
RX8900RTC        rtc;
LIS2DW12Sensor   accel(&SPI, PIN_CS_ACC);

// ---------------------------------------------------------------------------
// Shared sensor state
// ---------------------------------------------------------------------------
struct SensorData {
  float ensTempC   = NAN;   // ENS210 ambient temperature
  float ensHum     = NAN;   // ENS210 relative humidity (%)
  float bmpTempC   = NAN;   // BMP580 die temperature
  float bmpPresPa  = NAN;   // BMP580 pressure (Pa)
  uint16_t co2     = 0;     // SCD41 CO2 (ppm)
  float scdTempC   = NAN;   // SCD41 temperature
  float scdHum     = NAN;   // SCD41 humidity (%)
  int32_t accel_mg[3] = {0, 0, 0};
  bool scdValid    = false;
} data;

// ---------------------------------------------------------------------------
// Display state
// ---------------------------------------------------------------------------
static const int CANVAS_W = 32;         // four 8x8 panels side by side
static const int CANVAS_H = 8;

enum Mode : uint8_t { MODE_CLOCK = 0, MODE_FIRE, MODE_PARTICLES, MODE_COUNT };
uint8_t mode = MODE_CLOCK;

uint16_t clockColour = LED_GREEN;
uint8_t  brightness  = 8;               // 0..15
bool     particlesReady = false;        // fluid sim re-seeds when false

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
unsigned long lastSensorRead = 0;
unsigned long lastTempCal     = 0;
const unsigned long SENSOR_PERIOD_MS = 2000;
const unsigned long TEMP_CAL_MS      = 10UL * 60UL * 1000UL;   // 10 min

// ===========================================================================
// Setup
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n[adapter] boot"));

  pinMode(PIN_BTN_S1, INPUT_PULLUP);
  pinMode(PIN_BTN_S2, INPUT_PULLUP);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS_ACC);

  initMatrices();
  initBMP();
  initENS210();
  initRTC();
  initAccel();
  initSCD();          // must run after BMP/ENS so we can seed compensation

  Serial.println(F("[adapter] init complete"));
}

// ===========================================================================
// Peripheral init helpers
// ===========================================================================
void initMatrices() {
  for (int i = 0; i < 4; i++) {
    matrix[i].begin(MATRIX_ADDR[i]);
    matrix[i].setRotation(0);          // adjust per physical orientation
    matrix[i].setBrightness(brightness);
    matrix[i].setTextWrap(false);
    matrix[i].setTextSize(1);
    matrix[i].clear();
    matrix[i].writeDisplay();
  }
  Serial.println(F("[matrix] 4x HT16K33 ready"));
}

void initBMP() {
  if (bmp.beginI2C(BMP581_I2C_ADDRESS_DEFAULT) != BMP5_OK) {
    Serial.println(F("[bmp] NOT found (0x47)"));
  } else {
    Serial.println(F("[bmp] ready"));
  }
}

void initENS210() {
  ens210.begin();
  Serial.println(F("[ens210] ready"));
}

void initRTC() {
  rtc.init();
  // Seed the RTC with the build time if it looks unset (before 2024).
  tmElements_t now = rtc.read();
  if (tmYearToCalendar(now.Year) < 2024) {
    time_t compiled = buildTime();
    if (compiled != 0) {
      rtc.set(compiled);
      Serial.println(F("[rtc] seeded with build time"));
    }
  }
  Serial.println(F("[rtc] ready"));
}

void initAccel() {
  if (accel.begin() != LIS2DW12_STATUS_OK) {
    Serial.println(F("[accel] begin failed"));
    return;
  }
  accel.Enable_X();
  Serial.println(F("[accel] ready"));
}

void initSCD() {
  // Do not auto-start measurement so we can program compensation first.
  if (scd.begin(false, true, false, true, Wire) == false) {
    Serial.println(F("[scd] NOT found (0x62)"));
    return;
  }
  scd.stopPeriodicMeasurement();       // ensure idle before configuring

  // Seed pressure compensation from BMP580 (falls back to sea level).
  bmp5_sensor_data bd = {0, 0};
  if (bmp.getSensorData(&bd) == BMP5_OK) {
    scd.setAmbientPressure(bd.pressure);        // Pascals
  } else {
    scd.setAmbientPressure(101325.0f);
  }

  // Initial self-heating temperature offset. Refined later against ENS210.
  scd.setTemperatureOffset(4.0f);

  scd.startPeriodicMeasurement();
  Serial.println(F("[scd] ready (periodic)"));
}

void loop() {
  handleButtons();

  unsigned long nowMs = millis();
  if (nowMs - lastSensorRead >= SENSOR_PERIOD_MS) {
    lastSensorRead = nowMs;
    readSensors();
    pushCompensation();
    logSensors();
  }

  if (nowMs - lastTempCal >= TEMP_CAL_MS) {
    lastTempCal = nowMs;
    recalibrateScdTempOffset();
  }

  // Render one animation frame for the active mode.
  renderMode();
}

// ===========================================================================
// Sensor acquisition
// ===========================================================================
void readSensors() {
  // ENS210 (blocking single-shot, ~130 ms)
  if (ens210.singleShotMeasure() == ENS210::Result::STATUS_OK) {
    data.ensTempC = ens210.getTempCelsius();
    data.ensHum   = ens210.getHumidityPercent();
  }

  // BMP580
  bmp5_sensor_data bd = {0, 0};
  if (bmp.getSensorData(&bd) == BMP5_OK) {
    data.bmpTempC  = bd.temperature;
    data.bmpPresPa = bd.pressure;
  }

  // SCD41 (fresh roughly every 5 s in periodic mode)
  if (scd.readMeasurement()) {
    data.co2      = scd.getCO2();
    data.scdTempC = scd.getTemperature();
    data.scdHum   = scd.getHumidity();
    data.scdValid = true;
  }

  // LIS2DW12
  accel.Get_X_Axes(data.accel_mg);
}

// Continuous pressure compensation for the SCD41 (allowed while measuring).
void pushCompensation() {
  if (!isnan(data.bmpPresPa) && data.bmpPresPa > 30000.0f) {
    scd.setAmbientPressure(data.bmpPresPa);
  }
}

// Periodically trim the SCD41 temperature offset so its reported temperature
// tracks the ENS210 ambient reading. Requires stopping periodic measurement.
void recalibrateScdTempOffset() {
  if (!data.scdValid || isnan(data.ensTempC) || isnan(data.scdTempC)) return;

  scd.stopPeriodicMeasurement();
  float current = scd.getTemperatureOffset();
  float delta   = data.scdTempC - data.ensTempC;   // how much hotter SCD reads
  float updated = current + delta;
  if (updated < 0)  updated = 0;
  if (updated > 20) updated = 20;
  scd.setTemperatureOffset(updated);
  scd.startPeriodicMeasurement();

  Serial.print(F("[scd] temp offset -> "));
  Serial.println(updated, 2);
}

// ===========================================================================
// Buttons
// ===========================================================================
void handleButtons() {
  static uint32_t lastS1 = 0, lastS2 = 0;
  uint32_t nowMs = millis();

  if (digitalRead(PIN_BTN_S1) == LOW && nowMs - lastS1 > 250) {
    lastS1 = nowMs;
    brightness = (brightness + 4) & 0x0F;   // step through 0,4,8,12
    for (int i = 0; i < 4; i++) matrix[i].setBrightness(brightness);
  }
  if (digitalRead(PIN_BTN_S2) == LOW && nowMs - lastS2 > 250) {
    lastS2 = nowMs;
    mode = (mode + 1) % MODE_COUNT;
    onModeChange();
  }
}

void onModeChange() {
  particlesReady = false;                // force re-seed of the fluid sim
  canvasClear();
  canvasShow();
  Serial.print(F("[mode] -> "));
  Serial.println(mode);
}

// ===========================================================================
// Canvas helpers: four 8x8 panels addressed as one 32x8 surface
// ===========================================================================
// Global x maps to panel (x / 8) at local column (x % 8). With rotation 0 the
// GFX drawPixel writes straight into that panel's row/column buffer.
inline void canvasClear() {
  for (int i = 0; i < 4; i++) matrix[i].clear();
}

inline void canvasShow() {
  for (int i = 0; i < 4; i++) matrix[i].writeDisplay();
}

inline void canvasPixel(int x, int y, uint16_t colour) {
  if (x < 0 || x >= CANVAS_W || y < 0 || y >= CANVAS_H) return;
  matrix[x >> 3].drawPixel(x & 7, y, colour);
}

void renderMode() {
  switch (mode) {
    case MODE_CLOCK:     renderClock();     break;
    case MODE_FIRE:      renderFire();      break;
    case MODE_PARTICLES: renderParticles(); break;
  }
}

// ---------------------------------------------------------------------------
// Mode 0: clock  (HH:MM centred on the 32x8 surface, colon blinks at 1 Hz)
// ---------------------------------------------------------------------------
void renderClock() {
  tmElements_t tm = rtc.read();
  bool colon = (millis() / 500) % 2;

  char buf[6];
  snprintf(buf, sizeof(buf), "%02d%c%02d",
           tm.Hour, colon ? ':' : ' ', tm.Minute);

  canvasClear();
  const int off = 1;                       // "HH:MM" = 30 px, 1 px left margin
  for (int i = 0; i < 4; i++) {
    matrix[i].setCursor(off - i * 8, 0);
    matrix[i].setTextColor(clockColour);
    matrix[i].print(buf);
  }
  canvasShow();
  delay(50);
}

// ---------------------------------------------------------------------------
// Mode 1: fire  (Doom-style upward propagation; hotter cells burn "orange")
// ---------------------------------------------------------------------------
uint8_t heat[CANVAS_H][CANVAS_W];

void renderFire() {
  // Seed the bottom row with fresh heat, flickering some cells low.
  for (int x = 0; x < CANVAS_W; x++) {
    heat[CANVAS_H - 1][x] = (random(100) < 80) ? 255 : (uint8_t)random(80, 160);
  }

  // Propagate heat upward with cooling and a little horizontal blur.
  for (int y = 0; y < CANVAS_H - 1; y++) {
    for (int x = 0; x < CANVAS_W; x++) {
      int b  = heat[y + 1][x];
      int bl = heat[y + 1][max(0, x - 1)];
      int br = heat[y + 1][min(CANVAS_W - 1, x + 1)];
      int v  = (b * 2 + bl + br) / 4 - (int)random(0, 42);
      heat[y][x] = v < 0 ? 0 : (uint8_t)v;
    }
  }

  canvasClear();
  for (int y = 0; y < CANVAS_H; y++) {
    for (int x = 0; x < CANVAS_W; x++) {
      uint8_t h = heat[y][x];
      if      (h >= 150) canvasPixel(x, y, LED_YELLOW);   // orange flame
      else if (h >=  55) canvasPixel(x, y, LED_RED);      // ember
      // else: cell stays off
    }
  }
  canvasShow();
  delay(55);
}

// ---------------------------------------------------------------------------
// Mode 2: particles  (gravity vector taken from the LIS2DW12 accelerometer)
// ---------------------------------------------------------------------------
static const int   NUM_PARTICLES = 26;
static const float GRAV        = 0.16f;   // gravity strength (tune to taste)
static const float DAMP        = 0.90f;   // velocity damping per frame
static const float RESTITUTION = 0.35f;   // wall bounciness
// Flip these to match the physical mounting of the accelerometer.
static const float ACC_X_SIGN = -1.0f;
static const float ACC_Y_SIGN =  1.0f;

float px[NUM_PARTICLES], py[NUM_PARTICLES];
float vx[NUM_PARTICLES], vy[NUM_PARTICLES];

void seedParticles() {
  for (int i = 0; i < NUM_PARTICLES; i++) {
    px[i] = random(0, CANVAS_W * 100) / 100.0f;
    py[i] = random(0, CANVAS_H * 100) / 100.0f;
    vx[i] = vy[i] = 0.0f;
  }
  particlesReady = true;
}

void renderParticles() {
  if (!particlesReady) seedParticles();

  // Live gravity direction from the accelerometer (values in mg -> g).
  int32_t a[3];
  accel.Get_X_Axes(a);
  float gx = ACC_X_SIGN * (a[0] / 1000.0f) * GRAV;
  float gy = ACC_Y_SIGN * (a[1] / 1000.0f) * GRAV;

  for (int i = 0; i < NUM_PARTICLES; i++) {
    vx[i] = (vx[i] + gx) * DAMP;
    vy[i] = (vy[i] + gy) * DAMP;
    px[i] += vx[i];
    py[i] += vy[i];

    if (px[i] < 0)              { px[i] = 0;              vx[i] = -vx[i] * RESTITUTION; }
    if (px[i] > CANVAS_W - 1)   { px[i] = CANVAS_W - 1;   vx[i] = -vx[i] * RESTITUTION; }
    if (py[i] < 0)              { py[i] = 0;              vy[i] = -vy[i] * RESTITUTION; }
    if (py[i] > CANVAS_H - 1)   { py[i] = CANVAS_H - 1;   vy[i] = -vy[i] * RESTITUTION; }
  }

  canvasClear();
  for (int i = 0; i < NUM_PARTICLES; i++) {
    canvasPixel((int)(px[i] + 0.5f), (int)(py[i] + 0.5f), LED_GREEN);
  }
  canvasShow();
  delay(30);
}

// ---------------------------------------------------------------------------
// Helper: current time as HH:MM:SS (used for serial logging)
// ---------------------------------------------------------------------------
String timeString() {
  tmElements_t tm = rtc.read();
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.Hour, tm.Minute, tm.Second);
  return String(buf);
}

// ===========================================================================
// Serial logging
// ===========================================================================
void logSensors() {
  Serial.print(F("[data] "));
  Serial.print(timeString());
  Serial.print(F("  ENS T="));  Serial.print(data.ensTempC, 2);
  Serial.print(F("C RH="));     Serial.print(data.ensHum, 1);
  Serial.print(F("%  BMP P=")); Serial.print(data.bmpPresPa / 100.0f, 1);
  Serial.print(F("hPa T="));    Serial.print(data.bmpTempC, 2);
  Serial.print(F("C  SCD CO2="));Serial.print(data.co2);
  Serial.print(F("ppm T="));    Serial.print(data.scdTempC, 2);
  Serial.print(F("C RH="));     Serial.print(data.scdHum, 1);
  Serial.print(F("%  ACC[mg]=")); Serial.print(data.accel_mg[0]);
  Serial.print(',');            Serial.print(data.accel_mg[1]);
  Serial.print(',');            Serial.println(data.accel_mg[2]);
}

// ===========================================================================
// Build-time -> time_t (used to seed the RTC on a fresh board)
// ===========================================================================
time_t buildTime() {
  const char *monthNames = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char monStr[4];
  int day, year, hour, minute, second;

  if (sscanf(__DATE__, "%3s %d %d", monStr, &day, &year) != 3) return 0;
  if (sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) return 0;

  int month = (strstr(monthNames, monStr) - monthNames) / 3 + 1;

  tmElements_t tm;
  tm.Year   = CalendarYrToTm(year);
  tm.Month  = month;
  tm.Day    = day;
  tm.Hour   = hour;
  tm.Minute = minute;
  tm.Second = second;
  return makeTime(tm);
}
