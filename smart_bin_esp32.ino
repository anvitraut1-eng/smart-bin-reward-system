/*
 * Smart Bin ESP32 - Municipal Waste Monitoring + Citizen Reward System
 *
 * Hardware:
 * - ESP32 DevKit
 * - HC-SR04/JSN-SR04T ultrasonic sensor (Trig, Echo)
 * - SW-420 vibration sensor (interrupt-capable GPIO)
 * - Push button for calibration (GPIO to GND, internal pull-up)
 * - RC522 RFID/NFC reader (SPI: SDA, SCK, MOSI, MISO, RST)
 *
 * Features:
 * - Calibration button for empty-bin baseline
 * - Fill % monitoring with ultrasonic sensor
 * - Vibration-pattern empty detection
 * - RFID tap → citizen reward on confirmed disposal
 * - Offline buffering, WiFi reconnect, NTP sync
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <MFRC522.h>
#include "time.h"

// ============================================================================
// CONFIGURATION - UPDATE THESE FOR YOUR DEPLOYMENT
// ============================================================================

// WiFi credentials
const char* WIFI_SSID = "VIPUl1";
const char* WIFI_PASSWORD = "vipul@india";

// Supabase configuration
const char* SUPABASE_URL = "https://bpecehlmvzuirxmruvyt.supabase.co/rest/v1";
const char* SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImJwZWNlaGxtdnp1aXJ4bXJ1dnl0Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODY4OTEzMzMsImV4cCI6MjEwMjQ2NzMzM30.LyOps1xtjd1mPVc7OL1e6xLVW6Uu7J7RSPiTEZbZJyU";

// Device ID (unique per bin)
String DEVICE_ID = "BIN_ESP32_001";  // Change this for each deployed bin

// Hardware pin configuration
#define TRIG_PIN 5
#define ECHO_PIN 18
#define VIBRATION_PIN 19
#define CALIBRATION_BUTTON_PIN 21
#define RC522_RST_PIN 22
#define RC522_SS_PIN 15

// Default bin height (cm) - overridden by calibration
const float DEFAULT_BIN_HEIGHT = 25.0;

// Reward system constants (v1 placeholder rules - CONFIRM BEFORE PRODUCTION)
const int POINTS_PER_DISPOSAL = 10;
const unsigned long DISPOSAL_WINDOW_MS = 10000;  // 10 seconds after tap
const unsigned long RATE_LIMIT_MS = 300000;      // 5 minutes
const float MIN_FILL_RISE_PCT = 2.0;             // Minimum rise to count as disposal

// NTP configuration
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 19800;  // IST = UTC+5:30
const int DAYLIGHT_OFFSET_SEC = 0;

// ============================================================================
// GLOBAL OBJECTS & STATE
// ============================================================================

Preferences preferences;
MFRC522 rfid(RC522_SS_PIN, RC522_RST_PIN);

// Calibration
float calibratedBaseline = DEFAULT_BIN_HEIGHT;
bool calibrationValid = false;

// Ultrasonic reading state
unsigned long lastReadingTime = 0;
const unsigned long READING_INTERVAL = 30000;  // 30 seconds
float currentFillPct = 0.0;

// Vibration detection state
volatile unsigned long lastVibrationTime = 0;
volatile int vibrationPulseCount = 0;
unsigned long motionWindowStart = 0;
unsigned long lastMotionTime = 0;
bool inMotionWindow = false;
const unsigned long MOTION_WINDOW_MS = 15000;
const unsigned long QUIET_PERIOD_MS = 3000;
const unsigned long SETTLE_MS = 5000;

// Empty detection state
float fillBeforeEmpty = 0.0;
bool emptyDetectionActive = false;

// Reward system state
struct RewardSession {
  bool active;
  String cardUID;
  unsigned long startTime;
  float fillAtTap;
  bool disposalDetected;
};
RewardSession rewardSession = {false, "", 0, 0.0, false};

// Rate limiting: store last reward time per card (simple in-memory cache, max 10 cards)
struct RateLimit {
  String cardUID;
  unsigned long lastRewardTime;
};
RateLimit rateLimitCache[10];
int rateLimitCacheSize = 0;

// Offline buffering
struct BufferedEvent {
  String endpoint;  // "bin_readings", "empty_events", "reward_events"
  String payload;
};
BufferedEvent eventBuffer[20];
int eventBufferSize = 0;

// Calibration button debounce
unsigned long lastButtonPress = 0;
const unsigned long BUTTON_DEBOUNCE_MS = 500;

// ============================================================================
// INTERRUPT SERVICE ROUTINE
// ============================================================================

void IRAM_ATTR vibrationISR() {
  lastVibrationTime = millis();
  vibrationPulseCount++;
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Median of 5 ultrasonic samples
float readUltrasonicMedian() {
  float samples[5];
  for (int i = 0; i < 5; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duration = pulseIn(ECHO_PIN, HIGH, 30000);  // 30ms timeout
    if (duration == 0) {
      samples[i] = calibratedBaseline;  // Timeout = assume empty
    } else {
      samples[i] = duration * 0.034 / 2.0;  // cm
    }
    delay(60);  // HC-SR04 needs ~60ms between readings
  }

  // Sort and return median
  for (int i = 0; i < 4; i++) {
    for (int j = i + 1; j < 5; j++) {
      if (samples[i] > samples[j]) {
        float temp = samples[i];
        samples[i] = samples[j];
        samples[j] = temp;
      }
    }
  }
  return samples[2];
}

// Calculate fill percentage
float calculateFillPct(float distance) {
  if (!calibrationValid) return 0.0;
  float fillPct = ((calibratedBaseline - distance) / calibratedBaseline) * 100.0;
  return constrain(fillPct, 0.0, 100.0);
}

// Get ISO8601 timestamp
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "1970-01-01T00:00:00Z";
  }
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buffer);
}

// ============================================================================
// NETWORK FUNCTIONS
// ============================================================================

void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    // Sync time
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    Serial.println("NTP time sync initiated");
  } else {
    Serial.println("\nWiFi connection failed - will retry later");
  }
}

bool postToSupabase(const char* table, String jsonPayload) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected - buffering event");
    return false;
  }

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/" + table;

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer", "return=minimal");

  int httpCode = http.POST(jsonPayload);

  bool success = (httpCode == 201 || httpCode == 200);

  if (success) {
    Serial.print("POST to ");
    Serial.print(table);
    Serial.println(" successful");
  } else {
    Serial.print("POST failed: ");
    Serial.println(httpCode);
    if (httpCode > 0) {
      Serial.println(http.getString());
    }
  }

  http.end();
  return success;
}

void bufferEvent(const char* endpoint, String payload) {
  if (eventBufferSize >= 20) {
    Serial.println("Event buffer full - dropping oldest");
    for (int i = 0; i < 19; i++) {
      eventBuffer[i] = eventBuffer[i + 1];
    }
    eventBufferSize = 19;
  }

  eventBuffer[eventBufferSize].endpoint = endpoint;
  eventBuffer[eventBufferSize].payload = payload;
  eventBufferSize++;

  Serial.print("Buffered event (");
  Serial.print(eventBufferSize);
  Serial.println(" total)");
}

void flushEventBuffer() {
  if (WiFi.status() != WL_CONNECTED || eventBufferSize == 0) return;

  Serial.print("Flushing ");
  Serial.print(eventBufferSize);
  Serial.println(" buffered events...");

  int flushed = 0;
  for (int i = 0; i < eventBufferSize; i++) {
    if (postToSupabase(eventBuffer[i].endpoint.c_str(), eventBuffer[i].payload)) {
      flushed++;
    } else {
      // Failed - keep remaining events in buffer
      for (int j = 0; j < eventBufferSize - i; j++) {
        eventBuffer[j] = eventBuffer[i + j];
      }
      eventBufferSize = eventBufferSize - i;
      Serial.print("Flush incomplete - ");
      Serial.print(eventBufferSize);
      Serial.println(" events remain buffered");
      return;
    }
  }

  eventBufferSize = 0;
  Serial.println("All buffered events flushed");
}

// ============================================================================
// CALIBRATION
// ============================================================================

void performCalibration() {
  Serial.println("\n=== CALIBRATION START ===");
  Serial.println("Ensure bin is EMPTY");

  // Blink LED or delay to give user time
  delay(1000);

  float distance = readUltrasonicMedian();

  if (distance > 5.0 && distance < 400.0) {  // Valid range
    calibratedBaseline = distance;
    calibrationValid = true;

    preferences.begin("smartbin", false);
    preferences.putFloat("baseline", calibratedBaseline);
    preferences.end();

    Serial.print("Calibration successful: ");
    Serial.print(calibratedBaseline);
    Serial.println(" cm");
  } else {
    Serial.print("Calibration failed - invalid distance: ");
    Serial.println(distance);
  }

  Serial.println("=== CALIBRATION END ===\n");
}

// ============================================================================
// BIN MONITORING
// ============================================================================

void takeBinReading() {
  float distance = readUltrasonicMedian();
  currentFillPct = calculateFillPct(distance);

  Serial.print("Fill: ");
  Serial.print(currentFillPct);
  Serial.print("% (distance: ");
  Serial.print(distance);
  Serial.println(" cm)");

  // Post to Supabase
  String payload = "{\"device_id\":\"" + DEVICE_ID +
                   "\",\"fill_pct\":" + String(currentFillPct, 1) +
                   ",\"timestamp\":\"" + getTimestamp() + "\"}";

  if (!postToSupabase("bin_readings", payload)) {
    bufferEvent("bin_readings", payload);
  }
}

// ============================================================================
// EMPTY DETECTION (vibration + fill drop)
// ============================================================================

void checkEmptyDetection() {
  unsigned long now = millis();

  // Check for vibration pulses
  if (now - lastVibrationTime < 100) {  // Active vibration
    if (!inMotionWindow) {
      // Start new motion window
      motionWindowStart = now;
      inMotionWindow = true;
      vibrationPulseCount = 0;
      fillBeforeEmpty = currentFillPct;
      Serial.println("Motion window started");
    }
    lastMotionTime = now;
  }

  // Check if motion window should end
  if (inMotionWindow && (now - lastMotionTime > QUIET_PERIOD_MS)) {
    // Quiet period ended - classify event
    int pulseCount = vibrationPulseCount;
    unsigned long activeDuration = lastMotionTime - motionWindowStart;

    Serial.print("Motion ended - pulses: ");
    Serial.print(pulseCount);
    Serial.print(", duration: ");
    Serial.print(activeDuration);
    Serial.println(" ms");

    // Classify: bump vs handling
    bool isHandling = (pulseCount > 3 && activeDuration > 1000);

    if (isHandling) {
      // Wait for settle, then measure fill
      Serial.println("Handling detected - settling...");
      delay(SETTLE_MS);

      float distance = readUltrasonicMedian();
      float fillAfter = calculateFillPct(distance);
      float fillDrop = fillBeforeEmpty - fillAfter;

      Serial.print("Fill before: ");
      Serial.print(fillBeforeEmpty);
      Serial.print("%, after: ");
      Serial.print(fillAfter);
      Serial.print("%, drop: ");
      Serial.println(fillDrop);

      String eventType;
      if (fillDrop > 10.0) {
        eventType = "emptied";
      } else if (fillDrop > 3.0) {
        eventType = "emptied_unconfirmed";
      } else {
        eventType = "handling_no_empty";
      }

      // Log to Supabase
      String payload = "{\"device_id\":\"" + DEVICE_ID +
                       "\",\"event_type\":\"" + eventType +
                       "\",\"fill_pct_before\":" + String(fillBeforeEmpty, 1) +
                       ",\"fill_pct_after\":" + String(fillAfter, 1) +
                       ",\"pulse_count\":" + String(pulseCount) +
                       ",\"active_duration_ms\":" + String(activeDuration) +
                       ",\"timestamp\":\"" + getTimestamp() + "\"}";

      if (!postToSupabase("empty_events", payload)) {
        bufferEvent("empty_events", payload);
      }

      currentFillPct = fillAfter;  // Update current state
    }

    inMotionWindow = false;
    vibrationPulseCount = 0;
  }
}

// ============================================================================
// REWARD SYSTEM
// ============================================================================

bool isRateLimited(String cardUID) {
  unsigned long now = millis();
  for (int i = 0; i < rateLimitCacheSize; i++) {
    if (rateLimitCache[i].cardUID == cardUID) {
      if (now - rateLimitCache[i].lastRewardTime < RATE_LIMIT_MS) {
        return true;
      } else {
        // Expired - update time
        rateLimitCache[i].lastRewardTime = now;
        return false;
      }
    }
  }

  // New card - add to cache
  if (rateLimitCacheSize < 10) {
    rateLimitCache[rateLimitCacheSize].cardUID = cardUID;
    rateLimitCache[rateLimitCacheSize].lastRewardTime = now;
    rateLimitCacheSize++;
  } else {
    // Cache full - replace oldest (simple FIFO)
    for (int i = 0; i < 9; i++) {
      rateLimitCache[i] = rateLimitCache[i + 1];
    }
    rateLimitCache[9].cardUID = cardUID;
    rateLimitCache[9].lastRewardTime = now;
  }

  return false;
}

void startRewardSession(String cardUID) {
  rewardSession.active = true;
  rewardSession.cardUID = cardUID;
  rewardSession.startTime = millis();
  rewardSession.fillAtTap = currentFillPct;
  rewardSession.disposalDetected = false;

  Serial.print("Reward session started for card: ");
  Serial.println(cardUID);
}

void checkDisposalInWindow() {
  if (!rewardSession.active) return;

  unsigned long now = millis();

  // Check if window expired
  if (now - rewardSession.startTime > DISPOSAL_WINDOW_MS) {
    // Window expired - no disposal detected
    Serial.println("Disposal window expired - no disposal detected");

    String payload = "{\"card_uid\":\"" + rewardSession.cardUID +
                     "\",\"device_id\":\"" + DEVICE_ID +
                     "\",\"fill_pct_before\":" + String(rewardSession.fillAtTap, 1) +
                     ",\"fill_pct_after\":" + String(currentFillPct, 1) +
                     ",\"points_awarded\":0" +
                     ",\"confidence\":\"no_disposal\"" +
                     ",\"timestamp\":\"" + getTimestamp() + "\"}";

    if (!postToSupabase("reward_events", payload)) {
      bufferEvent("reward_events", payload);
    }

    rewardSession.active = false;
    return;
  }

  // Check for fill rise + handling motion
  if (!rewardSession.disposalDetected) {
    // Look for vibration indicating disposal action
    if (inMotionWindow && (now - lastVibrationTime < 100)) {
      // Active handling during window
      int pulseCount = vibrationPulseCount;
      unsigned long activeDuration = now - motionWindowStart;

      if (pulseCount > 2 && activeDuration > 500) {  // Disposal motion threshold
        // Wait briefly for fill to rise
        delay(1000);

        float distance = readUltrasonicMedian();
        float fillAfter = calculateFillPct(distance);
        float fillRise = fillAfter - rewardSession.fillAtTap;

        Serial.print("Fill at tap: ");
        Serial.print(rewardSession.fillAtTap);
        Serial.print("%, current: ");
        Serial.print(fillAfter);
        Serial.print("%, rise: ");
        Serial.println(fillRise);

        if (fillRise >= MIN_FILL_RISE_PCT) {
          // Confirmed disposal!
          Serial.println("DISPOSAL CONFIRMED - awarding points");

          String payload = "{\"card_uid\":\"" + rewardSession.cardUID +
                           "\",\"device_id\":\"" + DEVICE_ID +
                           "\",\"fill_pct_before\":" + String(rewardSession.fillAtTap, 1) +
                           ",\"fill_pct_after\":" + String(fillAfter, 1) +
                           ",\"points_awarded\":" + String(POINTS_PER_DISPOSAL) +
                           ",\"confidence\":\"confirmed\"" +
                           ",\"timestamp\":\"" + getTimestamp() + "\"}";

          if (!postToSupabase("reward_events", payload)) {
            bufferEvent("reward_events", payload);
          }

          rewardSession.disposalDetected = true;
          rewardSession.active = false;
          currentFillPct = fillAfter;
        }
      }
    }
  }
}

void checkRFIDTap() {
  // Check if a card is present
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  // Read UID
  String cardUID = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    cardUID += String(rfid.uid.uidByte[i], HEX);
  }
  cardUID.toUpperCase();

  Serial.print("RFID tap detected: ");
  Serial.println(cardUID);

  // Halt PICC
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  // Check rate limit
  if (isRateLimited(cardUID)) {
    Serial.println("Rate limited - tap ignored");

    String payload = "{\"card_uid\":\"" + cardUID +
                     "\",\"device_id\":\"" + DEVICE_ID +
                     "\",\"fill_pct_before\":" + String(currentFillPct, 1) +
                     ",\"fill_pct_after\":" + String(currentFillPct, 1) +
                     ",\"points_awarded\":0" +
                     ",\"confidence\":\"rate_limited\"" +
                     ",\"timestamp\":\"" + getTimestamp() + "\"}";

    if (!postToSupabase("reward_events", payload)) {
      bufferEvent("reward_events", payload);
    }

    return;
  }

  // Start reward session
  startRewardSession(cardUID);
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== Smart Bin ESP32 Starting ===");

  // Load calibration from flash
  preferences.begin("smartbin", true);
  calibratedBaseline = preferences.getFloat("baseline", DEFAULT_BIN_HEIGHT);
  preferences.end();

  if (calibratedBaseline > 5.0 && calibratedBaseline < 400.0) {
    calibrationValid = true;
    Serial.print("Loaded calibration: ");
    Serial.print(calibratedBaseline);
    Serial.println(" cm");
  } else {
    calibratedBaseline = DEFAULT_BIN_HEIGHT;
    Serial.println("No valid calibration - using default");
  }

  // Pin setup
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(CALIBRATION_BUTTON_PIN, INPUT_PULLUP);
  pinMode(VIBRATION_PIN, INPUT);

  // Vibration interrupt
  attachInterrupt(digitalPinToInterrupt(VIBRATION_PIN), vibrationISR, RISING);

  // Initialize SPI and RC522
  SPI.begin();
  rfid.PCD_Init();
  Serial.println("RC522 initialized");

  // WiFi connect
  connectWiFi();

  // Register device (upsert)
  if (WiFi.status() == WL_CONNECTED) {
    String payload = "{\"device_id\":\"" + DEVICE_ID +
                     "\",\"location\":\"Not Set\"}";
    postToSupabase("devices", payload);
  }

  Serial.println("=== Setup Complete ===\n");

  // Take initial reading
  takeBinReading();
  lastReadingTime = millis();
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void loop() {
  unsigned long now = millis();

  // WiFi reconnect check
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;
    if (now - lastReconnect > 30000) {  // Try every 30s
      Serial.println("WiFi disconnected - reconnecting...");
      connectWiFi();
      lastReconnect = now;

      if (WiFi.status() == WL_CONNECTED) {
        flushEventBuffer();
      }
    }
  } else {
    // Flush any buffered events
    if (eventBufferSize > 0) {
      flushEventBuffer();
    }
  }

  // Calibration button check
  if (digitalRead(CALIBRATION_BUTTON_PIN) == LOW) {
    if (now - lastButtonPress > BUTTON_DEBOUNCE_MS) {
      lastButtonPress = now;
      performCalibration();
    }
  }

  // Periodic bin reading
  if (now - lastReadingTime > READING_INTERVAL) {
    takeBinReading();
    lastReadingTime = now;
  }

  // Empty detection (vibration + fill drop)
  checkEmptyDetection();

  // RFID tap detection
  checkRFIDTap();

  // Check disposal window if reward session active
  if (rewardSession.active) {
    checkDisposalInWindow();
  }

  delay(10);  // Small delay to prevent tight loop
}
