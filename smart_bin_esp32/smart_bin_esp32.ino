/*
 * SMART BIN ESP32 - FINAL SCHOOL PROJECT FIRMWARE
 *
 * Hardware / pins:
 * HC-SR04: TRIG=5, ECHO=4
 * SW-420:  DO=27
 * Button:  GPIO21 -> GND (INPUT_PULLUP)
 * RC522:   SCK=18, MISO=19, MOSI=23, SS=15, RST=22
 * LED:     GPIO2
 *
 * Put WiFi + Supabase credentials in arduino_secrets.h:
 * #define WIFI_SSID_VALUE "..."
 * #define WIFI_PASSWORD_VALUE "..."
 * #define SUPABASE_ANON_KEY_VALUE "..."
 *
 * Device registration is intentionally skipped at boot. The device rows
 * already exist in Supabase, and the old registration request was blocked
 * by RLS. Readings and events are still sent normally.
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
const char* SUPABASE_ANON_KEY = SUPABASE_ANON_KEY_VALUE;
const char* SUPABASE_URL = "https://bpecehlmvzuirxmruvyt.supabase.co/rest/v1";

// Change only this for another physical bin.
String DEVICE_ID = "BIN_ESP32_001";

#define TRIG_PIN 5
#define ECHO_PIN 4
#define VIBRATION_PIN 27
#define CALIBRATION_BUTTON_PIN 21
#define RC522_RST_PIN 22
#define RC522_SS_PIN 15
#define ONBOARD_LED_PIN 2

const float DEFAULT_BIN_HEIGHT = 25.0f;
const float MIN_VALID_DISTANCE_CM = 2.0f;
const float MAX_VALID_DISTANCE_CM = 400.0f;

const unsigned long READING_INTERVAL_MS = 30000UL;
const unsigned long BUTTON_DEBOUNCE_MS = 60UL;
const unsigned long QUIET_PERIOD_MS = 3000UL;
const unsigned long SETTLE_MS = 1000UL;
const unsigned long WIFI_RETRY_MS = 30000UL;

const int POINTS_PER_DISPOSAL = 10;
const unsigned long DISPOSAL_WINDOW_MS = 10000UL;
const unsigned long RATE_LIMIT_MS = 300000UL;
const float MIN_FILL_RISE_PCT = 2.0f;

Preferences preferences;
MFRC522 rfid(RC522_SS_PIN, RC522_RST_PIN);

float calibratedBaseline = DEFAULT_BIN_HEIGHT;
bool calibrationValid = false;
float currentFillPct = 0.0f;
unsigned long lastReadingTime = 0;
unsigned long lastWiFiRetry = 0;

volatile uint32_t vibrationPulseCount = 0;
volatile unsigned long lastVibrationTime = 0;
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

struct RateLimitEntry {
  String cardUID;
  unsigned long lastRewardTime;
};
RateLimitEntry rateLimitCache[10];
int rateLimitCacheSize = 0;

struct BufferedEvent {
  String endpoint;
  String payload;
};
BufferedEvent eventBuffer[20];
int eventBufferSize = 0;

bool lastButtonState = HIGH;
unsigned long lastButtonChange = 0;

void setLED(bool on) {
  digitalWrite(ONBOARD_LED_PIN, on ? HIGH : LOW);
}

void blinkLED(int count, int onMs = 150, int offMs = 150) {
  for (int i = 0; i < count; i++) {
    setLED(true);
    delay(onMs);
    setLED(false);
    if (i + 1 < count) delay(offMs);
  }
}

void IRAM_ATTR vibrationISR() {
  vibrationPulseCount++;
  lastVibrationTime = millis();
  vibrationPulsePending = true;
}

float readUltrasonicMedian() {
  float samples[5];
  int validCount = 0;

  for (int i = 0; i < 5; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);

    if (duration > 0) {
      float distance = duration * 0.0343f / 2.0f;
      if (distance >= MIN_VALID_DISTANCE_CM && distance <= MAX_VALID_DISTANCE_CM) {
        samples[validCount++] = distance;
      }
    }

    delay(60);
  }

  if (validCount < 3) return NAN;

  for (int i = 0; i < validCount - 1; i++) {
    for (int j = i + 1; j < validCount; j++) {
      if (samples[i] > samples[j]) {
        float temp = samples[i];
        samples[i] = samples[j];
        samples[j] = temp;
      }
    }
  }

  return samples[validCount / 2];
}

float calculateFillPct(float distance) {
  if (!calibrationValid || isnan(distance) || calibratedBaseline <= 0.0f) return NAN;
  float fill = ((calibratedBaseline - distance) / calibratedBaseline) * 100.0f;
  return constrain(fill, 0.0f, 100.0f);
}

String getTimestampUTC() {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 1000)) return "1970-01-01T00:00:00Z";
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeInfo);
  return String(buffer);
}

void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println("NTP synchronization started.");
  } else {
    Serial.println("WiFi connection failed - will retry later.");
  }
}

bool postToSupabase(const char* table, const String& payload) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/" + table;

  if (!http.begin(url)) return false;

  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer", "return=minimal");

  int code = http.POST(payload);
  bool success = code >= 200 && code < 300;

  if (!success) {
    Serial.print("Supabase POST failed: ");
    Serial.println(code);
    if (code > 0) Serial.println(http.getString());
  }

  http.end();
  return success;
}

void bufferEvent(const char* endpoint, const String& payload) {
  if (eventBufferSize >= 20) {
    for (int i = 0; i < 19; i++) eventBuffer[i] = eventBuffer[i + 1];
    eventBufferSize = 19;
    Serial.println("Event buffer full - dropped oldest event.");
  }

  eventBuffer[eventBufferSize].endpoint = endpoint;
  eventBuffer[eventBufferSize].payload = payload;
  eventBufferSize++;
}

void sendOrBuffer(const char* endpoint, const String& payload) {
  if (!postToSupabase(endpoint, payload)) bufferEvent(endpoint, payload);
}

void flushEventBuffer() {
  if (WiFi.status() != WL_CONNECTED) return;

  while (eventBufferSize > 0) {
    if (!postToSupabase(eventBuffer[0].endpoint.c_str(), eventBuffer[0].payload)) return;

    for (int i = 0; i < eventBufferSize - 1; i++) {
      eventBuffer[i] = eventBuffer[i + 1];
    }
    eventBufferSize--;
  }
}

void loadCalibration() {
  if (!preferences.begin("smartbin", true)) {
    Serial.println("Preferences unavailable.");
    return;
  }

  float saved = preferences.getFloat("baseline", NAN);
  preferences.end();

  if (!isnan(saved) && saved > 5.0f && saved < 400.0f) {
    calibratedBaseline = saved;
    calibrationValid = true;
    Serial.print("Loaded saved calibration: ");
    Serial.print(saved, 1);
    Serial.println(" cm");
  } else {
    Serial.println("No valid saved calibration.");
    Serial.println("Press calibration button once with EMPTY bin.");
  }
}

bool performCalibration() {
  Serial.println();
  Serial.println("================================");
  Serial.println("CALIBRATION START");
  Serial.println("KEEP BIN EMPTY AND STILL");

  // Two blinks = calibration started.
  blinkLED(2, 100, 100);
  delay(1000);

  float distance = readUltrasonicMedian();

  if (isnan(distance) || distance <= 5.0f || distance >= 400.0f) {
    Serial.println("CALIBRATION FAILED - invalid ultrasonic reading.");
    blinkLED(5, 100, 100);
    Serial.println("================================");
    return false;
  }

  calibratedBaseline = distance;
  calibrationValid = true;

  if (preferences.begin("smartbin", false)) {
    preferences.putFloat("baseline", calibratedBaseline);
    preferences.end();
  }

  currentFillPct = 0.0f;

  Serial.print("Calibration successful: ");
  Serial.print(calibratedBaseline, 1);
  Serial.println(" cm");
  Serial.println("SUCCESS - LED blinking 3 times");
  blinkLED(3, 180, 180);
  Serial.println("CALIBRATION END");
  Serial.println("================================");
  Serial.println();

  return true;
}

void checkCalibrationButton() {
  bool state = digitalRead(CALIBRATION_BUTTON_PIN);
  unsigned long now = millis();

  if (state != lastButtonState && now - lastButtonChange >= BUTTON_DEBOUNCE_MS) {
    lastButtonChange = now;
    lastButtonState = state;

    // Trigger only on HIGH -> LOW press edge.
    if (state == LOW) performCalibration();
  }
}

void takeBinReading() {
  float distance = readUltrasonicMedian();

  if (isnan(distance)) {
    Serial.println("Ultrasonic read failed - no valid echo.");
    return;
  }

  float fill = calculateFillPct(distance);

  if (isnan(fill)) {
    Serial.println("No valid calibration - skipping Supabase reading.");
    return;
  }

  currentFillPct = fill;

  Serial.print("Fill: ");
  Serial.print(fill, 1);
  Serial.print("% | Distance: ");
  Serial.print(distance, 1);
  Serial.println(" cm");

  String payload =
    "{\"device_id\":\"" + DEVICE_ID +
    "\",\"fill_pct\":" + String(fill, 1) +
    ",\"timestamp\":\"" + getTimestampUTC() + "\"}";

  sendOrBuffer("bin_readings", payload);
}

bool isRateLimited(const String& uid) {
  unsigned long now = millis();

  for (int i = 0; i < rateLimitCacheSize; i++) {
    if (rateLimitCache[i].cardUID == uid) {
      return now - rateLimitCache[i].lastRewardTime < RATE_LIMIT_MS;
    }
  }

  return false;
}

void recordSuccessfulReward(const String& uid) {
  unsigned long now = millis();

  for (int i = 0; i < rateLimitCacheSize; i++) {
    if (rateLimitCache[i].cardUID == uid) {
      rateLimitCache[i].lastRewardTime = now;
      return;
    }
  }

  if (rateLimitCacheSize < 10) {
    rateLimitCache[rateLimitCacheSize].cardUID = uid;
    rateLimitCache[rateLimitCacheSize].lastRewardTime = now;
    rateLimitCacheSize++;
    return;
  }

  int oldest = 0;
  for (int i = 1; i < 10; i++) {
    if (rateLimitCache[i].lastRewardTime < rateLimitCache[oldest].lastRewardTime) oldest = i;
  }

  rateLimitCache[oldest].cardUID = uid;
  rateLimitCache[oldest].lastRewardTime = now;
}

void updateMotion() {
  unsigned long now = millis();

  uint32_t pulses;
  unsigned long lastPulse;
  bool pending;

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

    Serial.print("Motion ended - pulses: ");
    Serial.print(windowPulses);
    Serial.print(" | duration: ");
    Serial.print(duration);
    Serial.println(" ms");

    bool handling = windowPulses > 3 && duration > 1000;

    if (!rewardSession.active && handling) {
      Serial.println("Handling detected - checking for emptying...");
      delay(SETTLE_MS);

      float distance = readUltrasonicMedian();

      if (!isnan(distance)) {
        float fillAfter = calculateFillPct(distance);

        if (!isnan(fillAfter)) {
          float fillDrop = fillBeforeMotion - fillAfter;
          String eventType;

          if (fillDrop > 10.0f) eventType = "emptied";
          else if (fillDrop > 3.0f) eventType = "emptied_unconfirmed";
          else eventType = "handling_no_empty";

          String payload =
            "{\"device_id\":\"" + DEVICE_ID +
            "\",\"event_type\":\"" + eventType +
            "\",\"fill_pct_before\":" + String(fillBeforeMotion, 1) +
            ",\"fill_pct_after\":" + String(fillAfter, 1) +
            ",\"pulse_count\":" + String(windowPulses) +
            ",\"active_duration_ms\":" + String(duration) +
            ",\"timestamp\":\"" + getTimestampUTC() + "\"}";

          sendOrBuffer("empty_events", payload);
          currentFillPct = fillAfter;
        }
      }
    }

    inMotionWindow = false;
    motionStartPulseCount = pulses;
  }
}

void logRewardEvent(
  const String& uid,
  const String& confidence,
  int points,
  float beforeFill,
  float afterFill
) {
  String payload =
    "{\"card_uid\":\"" + uid +
    "\",\"device_id\":\"" + DEVICE_ID +
    "\",\"fill_pct_before\":" + String(beforeFill, 1) +
    ",\"fill_pct_after\":" + String(afterFill, 1) +
    ",\"points_awarded\":" + String(points) +
    ",\"confidence\":\"" + confidence +
    "\",\"timestamp\":\"" + getTimestampUTC() + "\"}";

  sendOrBuffer("reward_events", payload);
}

void startRewardSession(const String& uid) {
  rewardSession.active = true;
  rewardSession.cardUID = uid;
  rewardSession.startTime = millis();
  rewardSession.fillAtTap = currentFillPct;

  Serial.print("Reward session started for card: ");
  Serial.println(uid);
}

void checkRewardSession() {
  if (!rewardSession.active) return;

  unsigned long now = millis();

  if (now - rewardSession.startTime > DISPOSAL_WINDOW_MS) {
    Serial.println("Disposal window expired - no confirmed disposal.");

    logRewardEvent(
      rewardSession.cardUID,
      "no_disposal",
      0,
      rewardSession.fillAtTap,
      currentFillPct
    );

    rewardSession.active = false;
    return;
  }

  if (!inMotionWindow) return;

  uint32_t pulses;
  noInterrupts();
  pulses = vibrationPulseCount;
  interrupts();

  uint32_t windowPulses = pulses - motionStartPulseCount;
  unsigned long duration = now - motionWindowStart;

  if (windowPulses < 3 || duration < 500) return;

  Serial.println("Possible disposal detected - measuring fill...");

  // Take ownership of this motion event so it cannot be processed twice.
  inMotionWindow = false;

  delay(SETTLE_MS);

  float distance = readUltrasonicMedian();

  if (isnan(distance)) {
    Serial.println("Disposal check failed - ultrasonic read invalid.");
    return;
  }

  float fillAfter = calculateFillPct(distance);
  if (isnan(fillAfter)) return;

  float fillRise = fillAfter - rewardSession.fillAtTap;

  Serial.print("Fill at tap: ");
  Serial.print(rewardSession.fillAtTap, 1);
  Serial.print("% | After: ");
  Serial.print(fillAfter, 1);
  Serial.print("% | Rise: ");
  Serial.print(fillRise, 1);
  Serial.println("%");

  if (fillRise >= MIN_FILL_RISE_PCT) {
    Serial.println("DISPOSAL CONFIRMED - awarding points!");

    logRewardEvent(
      rewardSession.cardUID,
      "confirmed",
      POINTS_PER_DISPOSAL,
      rewardSession.fillAtTap,
      fillAfter
    );

    recordSuccessfulReward(rewardSession.cardUID);
    currentFillPct = fillAfter;
    rewardSession.active = false;

    // Two blinks = reward confirmed.
    blinkLED(2, 100, 100);
  } else {
    Serial.println("Disposal not confirmed - insufficient fill rise.");
    // Keep session alive for another attempt inside the remaining window.
  }
}

void checkRFIDTap() {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  String cardUID = "";

  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) cardUID += "0";
    cardUID += String(rfid.uid.uidByte[i], HEX);
  }

  cardUID.toUpperCase();

  Serial.print("RFID tap detected: ");
  Serial.println(cardUID);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (isRateLimited(cardUID)) {
    Serial.println("Card is rate limited.");

    logRewardEvent(
      cardUID,
      "rate_limited",
      0,
      currentFillPct,
      currentFillPct
    );

    blinkLED(1, 400, 100);
    return;
  }

  if (rewardSession.active) {
    Serial.println("Reward session already active.");
    return;
  }

  startRewardSession(cardUID);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("SMART BIN ESP32 STARTING");
  Serial.println("================================");

  pinMode(ONBOARD_LED_PIN, OUTPUT);
  setLED(false);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  pinMode(CALIBRATION_BUTTON_PIN, INPUT_PULLUP);
  lastButtonState = digitalRead(CALIBRATION_BUTTON_PIN);
  lastButtonChange = millis();

  pinMode(VIBRATION_PIN, INPUT);

  // Clear vibration state BEFORE enabling the interrupt.
  noInterrupts();
  vibrationPulseCount = 0;
  lastVibrationTime = 0;
  vibrationPulsePending = false;
  interrupts();

  SPI.begin();
  rfid.PCD_Init();
  delay(100);
  Serial.println("RC522 initialized.");

  loadCalibration();
  connectWiFi();

  // IMPORTANT: do not POST to /devices. Existing device rows are used.
  Serial.println("Device registration skipped - using existing device record.");

  if (calibrationValid) {
    takeBinReading();
  } else {
    Serial.println("Initial reading skipped - calibration required.");
  }

  lastReadingTime = millis();

  // One blink = startup complete.
  blinkLED(1, 150, 100);

  Serial.println();
  Serial.println("SMART BIN READY.");

  if (!calibrationValid) {
    Serial.println("Press calibration button ONCE with the bin EMPTY.");
  }

  Serial.println();
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWiFiRetry >= WIFI_RETRY_MS) {
      lastWiFiRetry = now;
      Serial.println("WiFi disconnected - reconnecting...");
      connectWiFi();
    }
  } else if (eventBufferSize > 0) {
    flushEventBuffer();
  }

  checkCalibrationButton();

  if (now - lastReadingTime >= READING_INTERVAL_MS) {
    takeBinReading();
    lastReadingTime = now;
  }

  updateMotion();
  checkRFIDTap();
  checkRewardSession();

  delay(10);
}
