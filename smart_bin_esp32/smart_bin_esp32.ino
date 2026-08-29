/* Smart Bin ESP32 - FIXED FIRMWARE
 * Pinout:
 * HC-SR04 TRIG=5, ECHO=4
 * SW-420 DO=27
 * Calibration button=21 (to GND)
 * RC522 SCK=18 MISO=19 MOSI=23 SS=15 RST=22
 * Onboard LED=2
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <MFRC522.h>
#include "time.h"
#include "arduino_secrets.h"

const char* WIFI_SSID = WIFI_SSID_VALUE;
const char* WIFI_PASSWORD = WIFI_PASSWORD_VALUE;
const char* SUPABASE_URL = "https://bpecehlmvzuirxmruvyt.supabase.co/rest/v1";
const char* SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImJwZWNlaGxtdnp1aXJ4bXJ1dnl0Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODY4OTEzMzMsImV4cCI6MjEwMjQ2NzMzM30.LyOps1xtjd1mPVc7OL1e6xLVW6Uu7J7RSPiTEZbZJyU";

String DEVICE_ID = "BIN_ESP32_001";

#define TRIG_PIN 5
#define ECHO_PIN 4
#define VIBRATION_PIN 27
#define CALIBRATION_BUTTON_PIN 21
#define RC522_RST_PIN 22
#define RC522_SS_PIN 15
#define ONBOARD_LED_PIN 2

const float DEFAULT_BIN_HEIGHT = 25.0f;
const float BIN_DIAMETER_CM = 30.0f;
const float WASTE_DENSITY_KG_M3 = 150.0f;
const int POINTS_PER_DISPOSAL = 10;
const unsigned long READING_INTERVAL = 30000;
const unsigned long DISPOSAL_WINDOW_MS = 10000;
const unsigned long RATE_LIMIT_MS = 300000;
const float MIN_FILL_RISE_PCT = 2.0f;
const unsigned long QUIET_PERIOD_MS = 3000;
const unsigned long SETTLE_MS = 5000;
const unsigned long BUTTON_DEBOUNCE_MS = 60;

Preferences preferences;
MFRC522 rfid(RC522_SS_PIN, RC522_RST_PIN);

float calibratedBaseline = DEFAULT_BIN_HEIGHT;
bool calibrationValid = false;
float currentFillPct = 0.0f;
unsigned long lastReadingTime = 0;

volatile unsigned long lastVibrationTime = 0;
volatile uint32_t vibrationPulseCount = 0;
volatile bool vibrationPulsePending = false;
uint32_t processedPulseCount = 0;

bool inMotionWindow = false;
unsigned long motionWindowStart = 0;
unsigned long lastMotionTime = 0;
uint32_t motionStartPulseCount = 0;
float fillBeforeMotion = 0.0f;

struct RewardSession {
  bool active;
  String cardUID;
  unsigned long startTime;
  float fillAtTap;
};
RewardSession rewardSession = {false, "", 0, 0.0f};

struct RateLimit {
  String cardUID;
  unsigned long lastRewardTime;
};
RateLimit rateLimitCache[10];
int rateLimitCacheSize = 0;

struct BufferedEvent { String endpoint; String payload; };
BufferedEvent eventBuffer[20];
int eventBufferSize = 0;

bool lastButtonState = HIGH;
unsigned long lastButtonChange = 0;

void IRAM_ATTR vibrationISR() {
  lastVibrationTime = millis();
  vibrationPulseCount++;
  vibrationPulsePending = true;
}

void led(bool on) { digitalWrite(ONBOARD_LED_PIN, on ? HIGH : LOW); }

void blink(int count, int onMs = 150, int offMs = 150) {
  for (int i = 0; i < count; i++) {
    led(true); delay(onMs); led(false);
    if (i + 1 < count) delay(offMs);
  }
}

float readUltrasonicMedian() {
  float samples[5];
  int n = 0;
  for (int i = 0; i < 5; i++) {
    digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    if (duration > 0) {
      float d = duration * 0.0343f / 2.0f;
      if (d >= 2.0f && d <= 400.0f) samples[n++] = d;
    }
    delay(60);
  }
  if (n < 3) return NAN;
  for (int i = 0; i < n - 1; i++) for (int j = i + 1; j < n; j++)
    if (samples[i] > samples[j]) { float t = samples[i]; samples[i] = samples[j]; samples[j] = t; }
  return samples[n / 2];
}

float fillFromDistance(float distance) {
  if (!calibrationValid || isnan(distance) || calibratedBaseline <= 0) return NAN;
  return constrain(((calibratedBaseline - distance) / calibratedBaseline) * 100.0f, 0.0f, 100.0f);
}

float estimateWeight(float fillRise) {
  float r = (BIN_DIAMETER_CM / 100.0f) / 2.0f;
  float h = calibratedBaseline / 100.0f;
  float volume = 3.14159f * r * r * h * (fillRise / 100.0f);
  float kg = volume * WASTE_DENSITY_KG_M3;
  return constrain(kg, 0.01f, 20.0f);
}

String timestampUTC() {
  struct tm t;
  if (!getLocalTime(&t, 1000)) return "1970-01-01T00:00:00Z";
  char b[30];
  strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(b);
}

void connectWiFi() {
  Serial.print("Connecting to WiFi: "); Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print('.'); }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: "); Serial.println(WiFi.localIP());
    configTime(0, 0, "pool.ntp.org");
    Serial.println("NTP synchronization started.");
  } else Serial.println("\nWiFi connection failed - will retry later.");
}

bool postToSupabase(const char* table, const String& payload, bool upsert = false) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  String url = String(SUPABASE_URL) + "/" + table;
  if (!http.begin(url)) return false;
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer", upsert ? "resolution=merge-duplicates, return=minimal" : "return=minimal");
  int code = http.POST(payload);
  bool ok = code >= 200 && code < 300;
  if (!ok) {
    Serial.print("Supabase POST failed: "); Serial.println(code);
    if (code > 0) Serial.println(http.getString());
  }
  http.end();
  return ok;
}

void bufferEvent(const char* endpoint, const String& payload) {
  if (eventBufferSize >= 20) {
    for (int i = 0; i < 19; i++) eventBuffer[i] = eventBuffer[i + 1];
    eventBufferSize = 19;
  }
  eventBuffer[eventBufferSize].endpoint = endpoint;
  eventBuffer[eventBufferSize].payload = payload;
  eventBufferSize++;
}

void flushEventBuffer() {
  while (WiFi.status() == WL_CONNECTED && eventBufferSize > 0) {
    if (!postToSupabase(eventBuffer[0].endpoint.c_str(), eventBuffer[0].payload)) return;
    for (int i = 0; i < eventBufferSize - 1; i++) eventBuffer[i] = eventBuffer[i + 1];
    eventBufferSize--;
  }
}

void loadCalibration() {
  if (!preferences.begin("smartbin", true)) return;
  float saved = preferences.getFloat("baseline", NAN);
  preferences.end();
  if (!isnan(saved) && saved > 5.0f && saved < 400.0f) {
    calibratedBaseline = saved; calibrationValid = true;
    Serial.print("Loaded saved calibration: "); Serial.print(saved, 1); Serial.println(" cm");
  } else Serial.println("No valid saved calibration - press button once to calibrate.");
}

bool performCalibration() {
  Serial.println("\n=== CALIBRATION START ===");
  Serial.println("Keep the bin EMPTY and still...");
  blink(2, 120, 120);
  delay(1000);
  float distance = readUltrasonicMedian();
  if (isnan(distance) || distance <= 5.0f || distance >= 400.0f) {
    Serial.println("Calibration FAILED - ultrasonic sensor returned invalid data.");
    blink(5, 100, 100);
    return false;
  }
  calibratedBaseline = distance;
  calibrationValid = true;
  if (preferences.begin("smartbin", false)) {
    preferences.putFloat("baseline", calibratedBaseline);
    preferences.end();
  }
  currentFillPct = 0.0f;
  Serial.print("Calibration successful: "); Serial.print(distance, 1); Serial.println(" cm");
  blink(3, 180, 180);
  Serial.println("=== CALIBRATION END ===\n");
  return true;
}

void checkCalibrationButton() {
  bool state = digitalRead(CALIBRATION_BUTTON_PIN);
  unsigned long now = millis();
  if (state != lastButtonState && now - lastButtonChange >= BUTTON_DEBOUNCE_MS) {
    lastButtonChange = now;
    lastButtonState = state;
    if (state == LOW) performCalibration();
  }
}

void takeBinReading() {
  float distance = readUltrasonicMedian();
  if (isnan(distance)) { Serial.println("Ultrasonic read failed - no valid echo."); return; }
  float fill = fillFromDistance(distance);
  if (isnan(fill)) return;
  currentFillPct = fill;
  Serial.print("Fill: "); Serial.print(fill, 1); Serial.print("% | Distance: "); Serial.print(distance, 1); Serial.println(" cm");
  String payload = "{\"device_id\":\"" + DEVICE_ID + "\",\"fill_pct\":" + String(fill, 1) + ",\"timestamp\":\"" + timestampUTC() + "\"}";
  if (!postToSupabase("bin_readings", payload)) bufferEvent("bin_readings", payload);
}

bool rateLimited(const String& uid) {
  unsigned long now = millis();
  for (int i = 0; i < rateLimitCacheSize; i++)
    if (rateLimitCache[i].cardUID == uid) return now - rateLimitCache[i].lastRewardTime < RATE_LIMIT_MS;
  return false;
}

void recordReward(const String& uid) {
  unsigned long now = millis();
  for (int i = 0; i < rateLimitCacheSize; i++) if (rateLimitCache[i].cardUID == uid) { rateLimitCache[i].lastRewardTime = now; return; }
  if (rateLimitCacheSize < 10) {
    rateLimitCache[rateLimitCacheSize++] = {uid, now};
    return;
  }
  int oldest = 0;
  for (int i = 1; i < 10; i++) if (rateLimitCache[i].lastRewardTime < rateLimitCache[oldest].lastRewardTime) oldest = i;
  rateLimitCache[oldest] = {uid, now};
}

void updateMotion() {
  unsigned long now = millis();
  uint32_t pulses; unsigned long lastPulse; bool pending;
  noInterrupts();
  pulses = vibrationPulseCount;
  lastPulse = lastVibrationTime;
  pending = vibrationPulsePending;
  vibrationPulsePending = false;
  interrupts();

  bool newPulse = pulses != processedPulseCount;
  if (newPulse || pending) {
    processedPulseCount = pulses;
    if (!inMotionWindow) {
      inMotionWindow = true;
      motionWindowStart = now;
      motionStartPulseCount = pulses;
      fillBeforeMotion = currentFillPct;
      Serial.println("Motion window started.");
    }
    lastMotionTime = lastPulse;
  }

  if (inMotionWindow && now - lastMotionTime > QUIET_PERIOD_MS) {
    uint32_t windowPulses = pulses - motionStartPulseCount;
    unsigned long duration = lastMotionTime - motionWindowStart;
    bool handling = windowPulses > 3 && duration > 1000;

    if (!rewardSession.active && handling) {
      Serial.println("Handling detected - settling...");
      delay(SETTLE_MS);
      float d = readUltrasonicMedian();
      float after = fillFromDistance(d);
      if (!isnan(after)) {
        float drop = fillBeforeMotion - after;
        String type = drop > 10.0f ? "emptied" : (drop > 3.0f ? "emptied_unconfirmed" : "handling_no_empty");
        String payload = "{\"device_id\":\"" + DEVICE_ID + "\",\"event_type\":\"" + type +
          "\",\"fill_pct_before\":" + String(fillBeforeMotion, 1) +
          ",\"fill_pct_after\":" + String(after, 1) +
          ",\"pulse_count\":" + String(windowPulses) +
          ",\"active_duration_ms\":" + String(duration) +
          ",\"timestamp\":\"" + timestampUTC() + "\"}";
        if (!postToSupabase("empty_events", payload)) bufferEvent("empty_events", payload);
        currentFillPct = after;
      }
    }
    inMotionWindow = false;
    motionStartPulseCount = pulses;
  }
}

void logRewardEvent(const String& uid, const String& confidence, int points) {
  String payload = "{\"card_uid\":\"" + uid + "\",\"device_id\":\"" + DEVICE_ID +
    "\",\"fill_pct_before\":" + String(currentFillPct, 1) +
    ",\"fill_pct_after\":" + String(currentFillPct, 1) +
    ",\"points_awarded\":" + String(points) +
    ",\"confidence\":\"" + confidence + "\",\"timestamp\":\"" + timestampUTC() + "\"}";
  if (!postToSupabase("reward_events", payload)) bufferEvent("reward_events", payload);
}

void startReward(const String& uid) {
  rewardSession = {true, uid, millis(), currentFillPct};
  Serial.print("Reward session started for card: "); Serial.println(uid);
}

void checkRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  String uid;
  for (byte i = 0; i < rfid.uid.size; i++) { if (rfid.uid.uidByte[i] < 0x10) uid += "0"; uid += String(rfid.uid.uidByte[i], HEX); }
  uid.toUpperCase();
  rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
  Serial.print("RFID tap detected: "); Serial.println(uid);

  if (rateLimited(uid)) { Serial.println("Rate limited - tap ignored."); logRewardEvent(uid, "rate_limited", 0); return; }
  if (rewardSession.active) { Serial.println("Reward session already active - tap ignored."); return; }
  startReward(uid);
}

void checkReward() {
  if (!rewardSession.active) return;
  unsigned long now = millis();
  if (now - rewardSession.startTime > DISPOSAL_WINDOW_MS) {
    Serial.println("Disposal window expired - no disposal detected.");
    logRewardEvent(rewardSession.cardUID, "no_disposal", 0);
    rewardSession.active = false;
    return;
  }
  if (!inMotionWindow) return;

  uint32_t pulses; unsigned long lastPulse;
  noInterrupts(); pulses = vibrationPulseCount; lastPulse = lastVibrationTime; interrupts();
  uint32_t windowPulses = pulses - motionStartPulseCount;
  unsigned long duration = now - motionWindowStart;
  if (now - lastPulse > 150 || windowPulses <= 2 || duration <= 500) return;

  delay(250);
  float d = readUltrasonicMedian();
  float after = fillFromDistance(d);
  if (isnan(after)) return;
  float rise = after - rewardSession.fillAtTap;
  Serial.print("Fill at tap: "); Serial.print(rewardSession.fillAtTap, 1); Serial.print("% | current: "); Serial.print(after, 1); Serial.print("% | rise: "); Serial.println(rise, 1);
  if (rise < MIN_FILL_RISE_PCT) return;

  float weight = estimateWeight(rise);
  String payload = "{\"card_uid\":\"" + rewardSession.cardUID + "\",\"device_id\":\"" + DEVICE_ID +
    "\",\"fill_pct_before\":" + String(rewardSession.fillAtTap, 1) +
    ",\"fill_pct_after\":" + String(after, 1) +
    ",\"weight_estimate_kg\":" + String(weight, 2) +
    ",\"points_awarded\":0,\"confidence\":\"pending_link\",\"timestamp\":\"" + timestampUTC() + "\"}";
  if (!postToSupabase("reward_events", payload)) bufferEvent("reward_events", payload);
  recordReward(rewardSession.cardUID);
  rewardSession.active = false;
  currentFillPct = after;
  Serial.println("DISPOSAL CONFIRMED.");
}

void registerDevice() {
  String payload = "{\"device_id\":\"" + DEVICE_ID + "\",\"location\":\"Not Set\"}";
  if (postToSupabase("devices", payload, true)) Serial.println("Device registered/upserted successfully.");
  else Serial.println("Device registration/upsert failed - continuing.");
}

void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n================================\nSMART BIN ESP32 STARTING\n================================");

  pinMode(ONBOARD_LED_PIN, OUTPUT); led(false);
  pinMode(TRIG_PIN, OUTPUT); digitalWrite(TRIG_PIN, LOW);
  pinMode(ECHO_PIN, INPUT);
  pinMode(VIBRATION_PIN, INPUT);
  pinMode(CALIBRATION_BUTTON_PIN, INPUT_PULLUP);
  lastButtonState = digitalRead(CALIBRATION_BUTTON_PIN);
  lastButtonChange = millis();

  loadCalibration();

  noInterrupts();
  vibrationPulseCount = 0;
  lastVibrationTime = 0;
  vibrationPulsePending = false;
  interrupts();
  processedPulseCount = 0;

  SPI.begin(); rfid.PCD_Init(); Serial.println("RC522 initialized.");
  attachInterrupt(digitalPinToInterrupt(VIBRATION_PIN), vibrationISR, RISING);

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) registerDevice();

  Serial.println("Setup complete.");
  if (calibrationValid) takeBinReading();
  else Serial.println("Waiting for calibration before reporting fill percentage.");
  lastReadingTime = millis();
  Serial.println("\nSMART BIN READY.");
  Serial.println("Press calibration button once with the bin EMPTY.");
}

void loop() {
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;
    if (now - lastReconnect >= 30000) {
      lastReconnect = now; Serial.println("WiFi disconnected - reconnecting..."); connectWiFi();
      if (WiFi.status() == WL_CONNECTED) { registerDevice(); flushEventBuffer(); }
    }
  } else if (eventBufferSize > 0) flushEventBuffer();

  checkCalibrationButton();
  if (calibrationValid && now - lastReadingTime >= READING_INTERVAL) { takeBinReading(); lastReadingTime = millis(); }
  updateMotion();
  checkRFID();
  checkReward();
  delay(10);
}
