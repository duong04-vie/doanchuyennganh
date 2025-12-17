/*
  ESP32 - Soil + Water Level + OLED + Firebase REST (no ArduinoJson)
  Works with your Android app (reads manual_override / pump_state / buzzer_state)
  Uses public Realtime DB (no auth). Make sure your rules allow read/write.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// ---------------- CONFIG ----------------
#define WIFI_SSID      "TP-Link_0528"
#define WIFI_PASSWORD  "34688946"
#define DATABASE_URL   "https://doan-5d191-default-rtdb.firebaseio.com" // no trailing '/'

// Hardware pins
#define SOIL_PIN    34   // ADC1_CH6
#define TRIG_PIN    18
#define ECHO_PIN    19
#define RELAY_PIN   17   // check relay active HIGH/LOW
#define BUZZER_PIN  16

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Thresholds
#define NGUONG_DAT_KHO 30
#define NGUONG_DAT_QUA_AM 85
#define NGUONG_MUC_NUOC 15  // cm

// Timing
const unsigned long SENSOR_INTERVAL = 5000UL;
const unsigned long FIREBASE_INTERVAL = 30000UL;
const int HISTORY_MAX = 20;

// Globals
unsigned long lastSensorTime = 0;
unsigned long lastFirebaseTime = 0;

// Manual control values received from app
bool manual_override = false;
bool pump_state_from_db = false;
bool buzzer_state_from_db = false;

// ---------- Helpers: HTTP to Firebase (no auth) ----------
String firebaseUrl(const String &pathAndQuery) {
  // pathAndQuery should start with '/'
  return String(DATABASE_URL) + pathAndQuery;
}

bool httpGET(const String &url, String &outBody, int timeout = 10000) {
  HTTPClient http;
  http.setTimeout(timeout);
  http.begin(url);
  int code = http.GET();
  if (code > 0) {
    outBody = http.getString();
    http.end();
    return true;
  } else {
    http.end();
    return false;
  }
}

bool httpPUT(const String &url, const String &payload, String &outBody, int timeout = 10000) {
  HTTPClient http;
  http.setTimeout(timeout);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(payload);
  if (code > 0) outBody = http.getString();
  http.end();
  return (code == 200 || code == 204 || code == 201);
}

bool httpDELETE(const String &url, String &outBody, int timeout = 10000) {
  HTTPClient http;
  http.setTimeout(timeout);
  http.begin(url);
  int code = http.sendRequest("DELETE");
  if (code > 0) outBody = http.getString();
  http.end();
  return (code == 200 || code == 204);
}

// ---------- Simple JSON helpers (string-based parsing) ----------
int countTopLevelKeys(const String &jsonObj) {
  // Expect "{}" or {"k1":...,"k2":...}
  // Return 0 if empty or not object.
  if (jsonObj.length() < 2) return 0;
  int count = 0;
  bool inQuotes = false;
  for (size_t i = 0; i < jsonObj.length(); ++i) {
    char c = jsonObj[i];
    if (c == '"') {
      // increment if this quote starts a key: check previous non-space is '{' or ','
      // find previous nonspace
      size_t j = i;
      while (j > 0) {
        --j;
        if (!isSpace(jsonObj[j])) break;
      }
      // If previous non-space is '{' or ',' then this is start of a key
      if (jsonObj[j] == '{' || jsonObj[j] == ',') count++;
      // skip to next quote
      ++i;
      while (i < jsonObj.length() && jsonObj[i] != '"') i++;
    }
  }
  return count;
}

String getFirstKeyFromObject(const String &jsonObj) {
  // parse first key name: assumes object like {"key":...}
  int firstQuote = jsonObj.indexOf('\"');
  if (firstQuote == -1) return "";
  int secondQuote = jsonObj.indexOf('\"', firstQuote + 1);
  if (secondQuote == -1) return "";
  return jsonObj.substring(firstQuote + 1, secondQuote);
}

bool jsonIsTrue(const String &body) {
  // body may be "true", "false", or "\"string\"", or number without quotes.
  String s = body;
  s.trim();
  if (s == "true") return true;
  return false;
}

long jsonToLong(const String &body) {
  String s = body;
  s.trim();
  // if quoted string, strip quotes
  if (s.startsWith("\"") && s.endsWith("\"") && s.length() >= 2) {
    s = s.substring(1, s.length() - 1);
  }
  return s.toInt();
}

// ---------- Sensors ----------
int readSoilPercent() {
  int adc = analogRead(SOIL_PIN); // 0..4095
  // calibrate these values on your sensor. Example values:
  const int DRY_ADC = 3900;
  const int WET_ADC = 1500;
  int pct = map(adc, DRY_ADC, WET_ADC, 0, 100);
  pct = constrain(pct, 0, 100);
  Serial.printf("Soil ADC=%d -> %d%%\n", adc, pct);
  return pct;
}

long readWaterLevel() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1;
  long cm = (long)(duration * 0.0343 / 2.0);
  return cm;
}

String getCurrentTimeForKey() {
  struct tm t;
  if (!getLocalTime(&t)) return String(millis()); // fallback
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &t);
  return String(buf);
}

String getNiceTime() {
  struct tm t;
  if (!getLocalTime(&t)) return "--:--:--";
  char buf[32];
  strftime(buf, sizeof(buf), "%H:%M:%S %d/%m", &t);
  return String(buf);
}

// ---------- Firebase-specific operations (unauth) ----------
bool writeRealtimeInt(const String &key, long value) {
  String url = firebaseUrl("/realtime_sensors/" + key + ".json");
  String payload = String(value);
  String resp;
  return httpPUT(url, payload, resp);
}

bool writeRealtimeBool(const String &key, bool value) {
  String url = firebaseUrl("/realtime_sensors/" + key + ".json");
  String payload = value ? "true" : "false";
  String resp;
  return httpPUT(url, payload, resp);
}

bool writeRealtimeString(const String &key, const String &value) {
  String url = firebaseUrl("/realtime_sensors/" + key + ".json");
  String payload = "\"" + value + "\"";
  String resp;
  return httpPUT(url, payload, resp);
}

bool pushHistoryEntry(int soil, long water, bool pump, bool buzzer) {
  String key = getCurrentTimeForKey();
  String url = firebaseUrl("/history/" + key + ".json");
  // build small JSON manually
  String payload = "{";
  payload += "\"soil_moisture\":" + String(soil) + ",";
  payload += "\"water_level\":" + String(water) + ",";
  payload += "\"pump_state\":" + String(pump ? "true" : "false") + ",";
  payload += "\"buzzer_state\":" + String(buzzer ? "true" : "false");
  payload += "}";
  String resp;
  return httpPUT(url, payload, resp);
}

int getHistoryCountShallow() {
  String url = firebaseUrl("/history.json?shallow=true");
  String body;
  if (!httpGET(url, body)) return -1;
  body.trim();
  if (body == "null" || body == "{}") return 0;
  int cnt = countTopLevelKeys(body);
  return cnt;
}

String getOldestHistoryKey() {
  // orderBy="$key"&limitToFirst=1 -> returns {"oldestKey": { ... }}
  String url = firebaseUrl("/history.json?orderBy=%22$key%22&limitToFirst=1");
  String body;
  if (!httpGET(url, body)) return "";
  body.trim();
  if (body.length() < 5) return "";
  return getFirstKeyFromObject(body);
}

bool deleteHistoryKey(const String &key) {
  if (key.length() == 0) return false;
  String url = firebaseUrl("/history/" + key + ".json");
  String resp;
  return httpDELETE(url, resp);
}

// Read manual control values from Firebase (no auth)
void fetchManualControlFromDB() {
  String body;
  // manual_override
  if (httpGET(firebaseUrl("/realtime_sensors/manual_override.json"), body)) {
    bool val = jsonIsTrue(body);
    manual_override = val;
  }
  // pump_state (when manual_override true, app writes pump_state to control pump)
  if (httpGET(firebaseUrl("/realtime_sensors/pump_state.json"), body)) {
    pump_state_from_db = jsonIsTrue(body);
  }
  // buzzer_state
  if (httpGET(firebaseUrl("/realtime_sensors/buzzer_state.json"), body)) {
    buzzer_state_from_db = jsonIsTrue(body);
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // OLED
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 init failed");
    for (;;) delay(10);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 10);
  display.println("Connecting WiFi...");
  display.display();

  // Connect WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
  } else {
    Serial.println("\nWiFi failed");
  }

  // NTP
  configTime(25200, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 30) {
    delay(200);
    retry++;
  }

  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("System Ready!");
  display.display();
  delay(800);
}

// ---------- Loop ----------
void loop() {
  unsigned long now = millis();

  // fetch manual control from DB more frequently
  if (now - lastSensorTime > 1000) {
    lastSensorTime = now;
    fetchManualControlFromDB();

    int soil = readSoilPercent();
    long water = readWaterLevel();

    bool pump_on = false;
    bool buzzer_on = false;
    String status = "";

    if (manual_override) {
      // When manual mode: follow pump_state_from_db & buzzer_state_from_db
      pump_on = pump_state_from_db;
      buzzer_on = buzzer_state_from_db;
      status = "Manual - Pump: " + String(pump_on ? "ON" : "OFF") + " Buzzer: " + String(buzzer_on ? "ON" : "OFF");
    } else {
      bool datKho = (soil < NGUONG_DAT_KHO);
      bool datQuaAm = (soil > NGUONG_DAT_QUA_AM);
      bool luLut = (water >= 0 && water < NGUONG_MUC_NUOC);

      if (luLut || datQuaAm) {
        pump_on = false;
        buzzer_on = true;
        status = luLut ? "! CANH BAO LU !" : "! SAT LO DAT !";
      } else {
        buzzer_on = false;
        pump_on = datKho;
        status = pump_on ? "Dang Tuoi Nuoc..." : "Trang thai tot";
      }
    }

    // Apply to hardware
    digitalWrite(RELAY_PIN, pump_on ? HIGH : LOW);
    digitalWrite(BUZZER_PIN, buzzer_on ? HIGH : LOW);

    // Update OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("Time: "); display.println(getNiceTime());
    display.setCursor(0, 15);
    display.printf("Do am dat: %d %%\n", soil);
    display.setCursor(0, 28);
    if (water >= 0) display.printf("Muc nuoc: %ld cm\n", water);
    else display.println("Muc nuoc: LOI");
    display.setCursor(0, 44);
    display.println(status);
    display.display();
  }

  // Push data to Firebase periodically
  if (WiFi.status() == WL_CONNECTED && (now - lastFirebaseTime > FIREBASE_INTERVAL)) {
    lastFirebaseTime = now;

    int soil = readSoilPercent();
    long water = readWaterLevel();
    bool pump_on = (digitalRead(RELAY_PIN) == HIGH);
    bool buzzer_on = (digitalRead(BUZZER_PIN) == HIGH);

    // Update realtime sensors
    writeRealtimeInt("doam", soil);
    writeRealtimeInt("mucnuoc", water);
    writeRealtimeString("last_update", getNiceTime());

    // If not manual override, write pump_state/buzzer_state reflecting automatic control
    if (!manual_override) {
      writeRealtimeBool("pump_state", pump_on);
      writeRealtimeBool("buzzer_state", buzzer_on);
    } else {
      // If manual_override true, we still write the current pump_state key so app stays consistent
      writeRealtimeBool("pump_state", pump_on);
      writeRealtimeBool("buzzer_state", buzzer_on);
    }

    // push history
    pushHistoryEntry(soil, water, pump_on, buzzer_on);

    // clean history if > HISTORY_MAX
    int cnt = getHistoryCountShallow();
    if (cnt > HISTORY_MAX) {
      Serial.printf("History count = %d, pruning...\n", cnt);
      String oldest = getOldestHistoryKey();
      if (oldest.length()) {
        if (deleteHistoryKey(oldest)) {
          Serial.printf("Deleted oldest: %s\n", oldest.c_str());
        } else {
          Serial.printf("Failed to delete oldest: %s\n", oldest.c_str());
        }
      } else {
        Serial.println("No oldest key found for deletion");
      }
    }
    Serial.println("Firebase sync done.");
  }

  // wifi reconnect non-blocking
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  }

  delay(10);
}
