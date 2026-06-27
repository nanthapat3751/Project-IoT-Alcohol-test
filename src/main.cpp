#include <Arduino.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>

const char *ssid = "Flame";
const char *password = "012345678";

const char *mqttServer = "mqtt.netpie.io";
const int mqttPort = 1883;
const char *mqttClientId = "6392d3f8-58ef-423a-8279-b74a510dd9cb";
const char *mqttUser = "MYzZcdkRcmgFSgsPoUYum7x69VQEbYPr";
const char *mqttPassword = "kNQG35mVsRaKhqLUErnJdVYckh7WDs5J";

const char *topic_sub = "@msg/home/device_control";
const char *topic_pub = "@msg/home/device_status";
const char *topic_pub_bac = "@msg/home/bac";
const char *data_pub = "@shadow/data/update";
const char *firebaseBaseUrl = "https://iotproject-64dc7-default-rtdb.asia-southeast1.firebasedatabase.app";

const int OLED_SDA = 21;
const int OLED_SCL = 22;
const int LED1_PIN = 4;
const int LED2_PIN = 19;
const int MQ3_PIN = 34;

const uint8_t SCREEN_WIDTH = 128; 
const uint8_t SCREEN_HEIGHT = 64;
const int OLED_RESET = -1;
const uint8_t OLED_ADDR = 0x3C;

const unsigned long SENSOR_READ_INTERVAL_MS = 1000; // อ่านเซ็นเซอร์ทุก 1 วินาที
const unsigned long DISPLAY_HOLD_MS = 2500; // แสดงค่าที่วัดได้ 2.5 วินาที ก่อนกลับไปหน้ารอวัดใหม่
const float MIN_REPORT_BAC = 10.0f;
const float BAC_MAX_VALUE = 100.0f;

float threshold1 = 20.0f; 
float threshold2 = 50.0f;
float lastBacValue = 0.0f; 
float lastDisplayedBac = 0.0f;
int lastRawValue = 0; 
int alertLevel = 0;
unsigned long lastSensorReadAt = 0;
unsigned long lastDisplayValueAt = 0;
bool displayReady = false;
bool displayHoldingValue = false;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// จำกัดค่า BAC ให้อยู่ในช่วงที่ระบบรองรับ
float clampBac(float value) {
  if (value < 0.0f) {
    return 0.0f;
  }
  if (value > BAC_MAX_VALUE) {
    return BAC_MAX_VALUE;
  }
  return value;
}

// ปัดทศนิยมให้เหลือ 1 ตำแหน่งเพื่อแสดงผล/ส่งข้อมูลให้สม่ำเสมอ
float roundOneDecimal(float value) {
  return roundf(value * 10.0f) / 10.0f;
}

// จัดระเบียบค่า threshold ให้อยู่ในช่วงถูกต้องและเรียงจากน้อยไปมาก
void normalizeThresholds() {
  threshold1 = clampBac(threshold1);
  threshold2 = clampBac(threshold2);

  // ถ้าสลับลำดับกันอยู่ ให้สลับกลับเพื่อกันเงื่อนไขผิดพลาด
  if (threshold1 > threshold2) {
    const float temp = threshold1;
    threshold1 = threshold2;
    threshold2 = temp;
  }
}

// อ่านค่าเซ็นเซอร์หลายครั้งแล้วเฉลี่ย เพื่อลดสัญญาณรบกวน
int readAlcoholRaw() {
  long total = 0;
  const int sampleCount = 10;

  for (int sample = 0; sample < sampleCount; sample++) {
    total += analogRead(MQ3_PIN);
    delay(5);
  }

  return total / sampleCount;
}

// แปลงค่าดิบเป็นเปอร์เซ็นต์  ตามสเกล 0-100
float convertRawToBac(int rawValue) {
  return clampBac((rawValue / 4095.0f) * BAC_MAX_VALUE);
}

// วาดข้อมูลสถานะทั้งหมดลง OLED
void renderDisplay(float bacValue, bool showMeasuredValue) {
  // ถ้า OLED ไม่พร้อม ให้ข้ามทันที
  if (!displayReady) {
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Alcohol Monitor");

  display.setTextSize(2);
  display.setCursor(0, 16);
  display.print("Alc: ");
  display.println(roundOneDecimal(showMeasuredValue ? bacValue : 0.0f), 1);

  display.setTextSize(1);
  display.setCursor(0, 42);
  display.print("T1:");
  display.print(roundOneDecimal(threshold1), 0);
  display.print(" T2:");
  display.println(roundOneDecimal(threshold2), 0);

  display.setCursor(0, 54);
  if (showMeasuredValue) {
    display.print("Sent to cloud");
  } else {
    display.print("Waiting for sample");
  }

  display.display();
}

// อัปเดตระดับแจ้งเตือน และควบคุม LED ตามค่า BAC
void updateAlertOutputs(float bacValue) {
  if (bacValue >= threshold2) {
    alertLevel = 2;
  } else if (bacValue >= threshold1) {
    alertLevel = 1;
  } else {
    alertLevel = 0;
  }

  digitalWrite(LED1_PIN, alertLevel >= 1 ? HIGH : LOW);
  digitalWrite(LED2_PIN, alertLevel >= 2 ? HIGH : LOW);
}

// ส่งสถานะล่าสุดขึ้น Shadow ของ NetPIE
void publishShadow() {
  if (!mqttClient.connected()) {
    Serial.println("Shadow publish skipped: MQTT disconnected");
    return;
  }

  const int led1State = digitalRead(LED1_PIN);
  const int led2State = digitalRead(LED2_PIN);

  StaticJsonDocument<384> doc;
  JsonObject data = doc.createNestedObject("data");
  data["red"] = led2State;
  data["yellow"] = led1State;
  data["led1"] = led1State;
  data["bac"] = roundOneDecimal(lastBacValue);
  data["threshold1"] = roundOneDecimal(threshold1);
  data["threshold2"] = roundOneDecimal(threshold2);

  char payload[384];
  serializeJson(doc, payload, sizeof(payload));
  const bool ok = mqttClient.publish(data_pub, payload, true);
  Serial.print("Shadow publish: ");
  Serial.println(ok ? "OK" : "FAIL");
}

// ส่งข้อมูลการวัดไปยัง topic สถานะและ topic BAC
void publishMeasurement() {
  if (!mqttClient.connected()) {
    Serial.println("MQTT publish skipped: MQTT disconnected");
    return;
  }

  StaticJsonDocument<256> doc;
  doc["bac"] = roundOneDecimal(lastBacValue);
  doc["raw"] = lastRawValue;
  doc["threshold1"] = roundOneDecimal(threshold1);
  doc["threshold2"] = roundOneDecimal(threshold2);
  doc["alertLevel"] = alertLevel;
  doc["millis"] = millis();

  char payload[256];
  serializeJson(doc, payload, sizeof(payload));
  const bool okStatus = mqttClient.publish(topic_pub, payload);
  const bool okBac = mqttClient.publish(topic_pub_bac, payload);
  Serial.print("MQTT publish status/bac: ");
  Serial.print(okStatus ? "OK" : "FAIL");
  Serial.print(" / ");
  Serial.println(okBac ? "OK" : "FAIL");
}

// ส่งค่าการวัดไปเก็บใน Firebase Realtime Database
void sendToFirebase() {
  // ส่งเฉพาะตอนต่อ WiFi ได้เท่านั้น
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient https;
  const String url = String(firebaseBaseUrl) + "/measurements.json";
  if (!https.begin(secureClient, url)) {
    Serial.println("Firebase connection failed");
    return;
  }

  https.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["bac"] = roundOneDecimal(lastBacValue);
  doc["raw"] = lastRawValue;
  doc["threshold1"] = roundOneDecimal(threshold1);
  doc["threshold2"] = roundOneDecimal(threshold2);
  doc["alertLevel"] = alertLevel;
  doc["millis"] = millis();

  String body;
  serializeJson(doc, body);
  const int httpCode = https.POST(body);
  Serial.print("Firebase HTTP code: ");
  Serial.println(httpCode);
  https.end();
}

// รับค่า threshold ใหม่จาก JSON และบังคับให้ค่าถูกต้องก่อนใช้งาน
void applyThresholdUpdate(JsonObjectConst source) {
  bool updated = false;

  if (source.containsKey("threshold1")) {
    threshold1 = source["threshold1"].as<float>();
    updated = true;
  }
  if (source.containsKey("threshold2")) {
    threshold2 = source["threshold2"].as<float>();
    updated = true;
  }
  if (source.containsKey("level1")) {
    threshold1 = source["level1"].as<float>();
    updated = true;
  }
  if (source.containsKey("level2")) {
    threshold2 = source["level2"].as<float>();
    updated = true;
  }

  if (!updated) {
    return;
  }

  normalizeThresholds();
  updateAlertOutputs(lastBacValue);
  publishShadow();
  renderDisplay(lastDisplayedBac, displayHoldingValue);

  Serial.print("Threshold updated: ");
  Serial.print(threshold1);
  Serial.print(", ");
  Serial.println(threshold2);
}

// callback เมื่อมีข้อความ MQTT เข้ามา
void callback(char *topic, byte *payload, unsigned int length) {
  (void)topic;

  String message;
  for (unsigned int index = 0; index < length; index++) {
    message += static_cast<char>(payload[index]);
  }

  Serial.print("Message arrived: ");
  Serial.println(message);

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    Serial.print("Invalid JSON: ");
    Serial.println(error.c_str());
    return;
  }

  JsonObjectConst source = doc.as<JsonObjectConst>();
  if (doc["data"].is<JsonObjectConst>()) {
    source = doc["data"].as<JsonObjectConst>();
  }

  applyThresholdUpdate(source);
}

// เชื่อมต่อ WiFi และรอจนกว่าจะสำเร็จ
void setupWifi() {
  delay(10);
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
}

// พยายามเชื่อม MQTT ใหม่จนกว่าจะสำเร็จ
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.println("Attempting MQTT connection...");
    if (mqttClient.connect(mqttClientId, mqttUser, mqttPassword)) {
      Serial.println("Connected to NetPIE");
      mqttClient.subscribe(topic_sub);
      publishShadow();
    } else {
      Serial.print("MQTT failed, rc=");
      Serial.println(mqttClient.state());
      delay(5000);
    }
  }
}

// เริ่มต้นจอ OLED และแสดงหน้าเริ่มต้น
void setupDisplay() {
  Wire.begin(OLED_SDA, OLED_SCL);
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);

  if (!displayReady) {
    Serial.println("OLED not found");
    return;
  }

  renderDisplay(0.0f, false);
}

// ตั้งค่าฮาร์ดแวร์และบริการทั้งหมดตอนบูต
void setup() {
  Serial.begin(115200);

  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  pinMode(MQ3_PIN, INPUT);

  digitalWrite(LED1_PIN, LOW);
  digitalWrite(LED2_PIN, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(MQ3_PIN, ADC_11db);
  normalizeThresholds();

  setupDisplay();
  setupWifi();

  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(callback);
  mqttClient.setBufferSize(512);
}

// วนลูปหลัก: ดูแลการเชื่อมต่อ, อ่านเซ็นเซอร์, อัปเดตแจ้งเตือน, และส่งข้อมูล
void loop() {
  // ถ้าหลุด MQTT ให้เชื่อมใหม่ก่อนทำงานต่อ
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  const unsigned long now = millis();
  // อ่านค่าเซ็นเซอร์ตามช่วงเวลา ไม่อ่านถี่เกินจำเป็น
  if (now - lastSensorReadAt >= SENSOR_READ_INTERVAL_MS) {
    lastSensorReadAt = now;

    lastRawValue = readAlcoholRaw();
    lastBacValue = roundOneDecimal(convertRawToBac(lastRawValue));
    updateAlertOutputs(lastBacValue);
    publishShadow();

    Serial.print("BAC: ");
    Serial.println(lastBacValue);

    // ส่งขึ้น cloud เฉพาะค่าที่เกินเกณฑ์ขั้นต่ำ
    if (lastBacValue > MIN_REPORT_BAC) {
      lastDisplayedBac = lastBacValue;
      lastDisplayValueAt = now;
      displayHoldingValue = true;
      renderDisplay(lastDisplayedBac, true);
      publishMeasurement();
      sendToFirebase();
    }
  }

  // ครบเวลาค้างหน้าจอแล้วกลับไปสถานะรอวัด
  if (displayHoldingValue && (now - lastDisplayValueAt >= DISPLAY_HOLD_MS)) {
    displayHoldingValue = false;
    lastDisplayedBac = 0.0f;
    renderDisplay(0.0f, false);
  }
}