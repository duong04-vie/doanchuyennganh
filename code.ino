#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <ArduinoJson.h> 
// ================== WIFI + FIREBASE ==================
#define WIFI_SSID      "TP-Link_0528"
#define WIFI_PASSWORD  "34688946"
#define DATABASE_URL   "https://doan-5d191-default-rtdb.firebaseio.com"

// ================== PIN ==================
#define SOIL_PIN    34
#define TRIG_PIN    18
#define ECHO_PIN    19
#define RELAY_PIN   17
#define BUZZER_PIN  16

// ================== OLED ==================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ================== NGUONG ==================
#define NGUONG_KHO        30
#define NGUONG_TOT        60
#define NGUONG_AM         85
#define NGUONG_MUC_NUOC   7   // cm

// ================== TIMER ==================
unsigned long lastLoop = 0;
unsigned long lastFirebase = 0;
#define FIREBASE_INTERVAL 30000

// ================== MANUAL ==================
bool manual_override = false;
bool pump_state_from_db = false;
bool buzzer_state_from_db = false;

// ================== FIREBASE HELPER ==================
String firebaseUrl(String path) {
  return String(DATABASE_URL) + path;
}

bool httpGET(String url, String &out) {
  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  if (code > 0) out = http.getString();
  http.end();
  return code > 0;
}

bool httpPUT(String url, String payload) {
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(payload);
  http.end();
  return (code == 200 || code == 201 || code == 204);
}

bool httpDELETE(String url) {
  HTTPClient http;
  http.begin(url);
  int code = http.sendRequest("DELETE");
  http.end();
  return (code == 200 || code == 201 || code == 204);
}

bool jsonIsTrue(String s) {
  s.trim();
  return (s == "true");
}

// ================== SENSOR ==================
int readSoilPercent() {
  int adc = analogRead(SOIL_PIN);
  const int DRY_ADC = 4095;
  const int WET_ADC = 1400;
  int pct = map(adc, DRY_ADC, WET_ADC, 0, 100);
  return constrain(pct, 0, 100);
}

String soilText(int soil) {
  if (soil < NGUONG_KHO) return "Kho";
  if (soil <= NGUONG_TOT) return "Tot";
  if (soil <= NGUONG_AM)  return "Am";
  return "Sat lo";
}

long readWaterLevel() {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long d = pulseIn(ECHO_PIN, HIGH, 30000);
  if (d == 0) return -1;
  return d * 0.0343 / 2;
}

String getTimeNow() {
  struct tm t;
  if (!getLocalTime(&t)) return "--:--:--";
  char buf[20];
  strftime(buf, sizeof(buf), "%H:%M:%S", &t);
  return String(buf);
}

String getDateTimeNow() {
  struct tm t;
  if (!getLocalTime(&t)) return "----/--/-- --:--:--";
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

String getTimestampKey() {
  struct tm t;
  if (!getLocalTime(&t)) return "0000-00-00-00-00-00";
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d-%H-%M-%S", &t);
  return String(buf);
}

// ================== MANUAL FETCH ==================
void fetchManualControl() {
  String body;
  if (httpGET(firebaseUrl("/realtime_sensors/manual_override.json"), body))
    manual_override = jsonIsTrue(body);

  if (httpGET(firebaseUrl("/realtime_sensors/pump_state.json"), body))
    pump_state_from_db = jsonIsTrue(body);

  if (httpGET(firebaseUrl("/realtime_sensors/buzzer_state.json"), body))
    buzzer_state_from_db = jsonIsTrue(body);
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  Wire.begin(21, 22);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println("Dang ket noi WiFi...");
  display.display();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(300);

  configTime(25200, 0, "pool.ntp.org");
}

// ================== LOOP ==================
void loop() {
  unsigned long now = millis();

  if (now - lastLoop > 1000) {
    lastLoop = now;

    fetchManualControl();

    int soil = readSoilPercent();
    String soil_state = soilText(soil);
    long water = readWaterLevel();

    bool pump = false;
    bool buzzer = false;
    String status = "";

    bool luLut = (water >= 0 && water < NGUONG_MUC_NUOC);

    if (manual_override) {
      pump = pump_state_from_db;
      buzzer = buzzer_state_from_db;
      status = "CHE DO TAY";

    } else {
      if (luLut) {
        pump = false;
        buzzer = true;
        status = "CANH BAO LU!";

      } else if (soil_state == "Sat lo") {
        pump = false;
        buzzer = true;
        status = "SAT LO DAT!";

      } else if (soil_state == "Kho") {
        pump = true;
        buzzer = false;
        status = "DANG TUOI NUOC";

      } else {
        pump = false;
        buzzer = false;
        status = "TRANG THAI ON";
      }
    }

    digitalWrite(RELAY_PIN, pump ? HIGH : LOW);
    digitalWrite(BUZZER_PIN, buzzer ? HIGH : LOW);

    // -------- OLED --------
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("Time: "); display.println(getTimeNow());

    display.setCursor(0, 16);
    display.print("Do am dat: ");
    display.println(soil_state);

    display.setCursor(0, 30);
    display.print("Muc nuoc: ");
    if (water >= 0) display.print(String(water) + " cm");
    else display.print("LOI");

    display.setCursor(0, 46);
    display.println(status);
    display.display();
  }

  // -------- FIREBASE --------
  if (WiFi.status() == WL_CONNECTED && now - lastFirebase > FIREBASE_INTERVAL) {
    lastFirebase = now;

    int soil = readSoilPercent();
    long water = readWaterLevel();
    String soil_text = soilText(soil);
    String datetime = getDateTimeNow();
    String key = getTimestampKey();

    httpPUT(firebaseUrl("/realtime_sensors/doam.json"), String(soil));
    httpPUT(firebaseUrl("/realtime_sensors/doam_text.json"), "\"" + soil_text + "\"");
    httpPUT(firebaseUrl("/realtime_sensors/mucnuoc.json"), String(water));
    httpPUT(firebaseUrl("/realtime_sensors/trangthai.json"), "\"" + String(digitalRead(BUZZER_PIN) ? "CANH BAO" : "BINH THUONG") + "\"");

    String shallow_url = firebaseUrl("/history.json?shallow=true");
    String shallow_body;
    size_t count = 0;
    if (httpGET(shallow_url, shallow_body)) {
      DynamicJsonDocument doc(2048);  // Du phong cho nhieu key
      DeserializationError error = deserializeJson(doc, shallow_body);
      if (!error) {
        count = doc.size();
      }
    }
    if (count >= 20) {
      String query_url = firebaseUrl("/history.json?orderBy=\"$key\"&limitToFirst=1");
      String query_body;
      if (httpGET(query_url, query_body)) {
        DynamicJsonDocument qdoc(1024);
        DeserializationError qerror = deserializeJson(qdoc, query_body);
        if (!qerror) {
          JsonObject root = qdoc.as<JsonObject>();
          if (root.size() > 0) {
            String old_key = root.begin()->key().c_str();
            String del_url = firebaseUrl("/history/" + old_key + ".json");
            httpDELETE(del_url);
          }
        }
      }
    }
    String payload = "{\"doam\":" + String(soil) + ", \"doam_text\":\"" + soil_text + "\", \"mucnuoc\":" + String(water) + ", \"datetime\":\"" + datetime + "\"}";
    String put_url = firebaseUrl("/history/" + key + ".json");
    httpPUT(put_url, payload);
    httpPUT(firebaseUrl("/realtime_sensors/last_update.json"), "\"" + datetime + "\"");
  }
  delay(10);
}
