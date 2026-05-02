/*
 * SLAVE SENSOR NODE - Industrial Safety Monitoring System
 * USING CHANNEL 5 for ESP-NOW (MUST MATCH MASTER)
 * COMPATIBLE WITH ESP32 CORE 3.x - NO CALLBACK VERSION
 * 
 * Master MAC: UPDATE WITH YOUR MASTER'S MAC ADDRESS
 */

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ========== OLED Display Configuration ==========
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define OLED_SDA 21
#define OLED_SCL 22
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ========== Pin Definitions ==========
#define DHTPIN 4
#define DHTTYPE DHT11
#define MQ2_PIN 34
#define FLAME_PIN 32
#define STATUS_LED 2
#define BUZZER_PIN 15

// ========== Section Configuration ==========
#define SECTION_ID 1  // Change to 1, 2, or 3 for different nodes

// ========== Thresholds ==========
#define TEMP_THRESHOLD 50.0
#define GAS_THRESHOLD 100  // 0-800 scale
#define MQ2_MAX_VALUE 800

// ========== Siren Configuration ==========
#define SIREN_FREQ_LOW 800
#define SIREN_FREQ_HIGH 1800
#define SIREN_SPEED 60

// ========== Master MAC Address ==========
// !!! IMPORTANT: UPDATE THIS WITH YOUR MASTER'S ACTUAL MAC ADDRESS !!!
// Run the master code first and copy the MAC address from serial monitor
uint8_t masterMac[] = {0x44, 0x1D, 0x64, 0xBD, 0x7B, 0x70};  // CHANGE THIS!

// ========== Data Structure (Matches Master) ==========
typedef struct struct_message {
  int sectionId;
  float temperature;
  int gasLevel;
  bool fireDetected;
  unsigned int readingId;
} struct_message;

struct_message sensorData;

// ========== Variables ==========
int mq2Baseline = 100;
unsigned long lastSendTime = 0;
const unsigned long SEND_INTERVAL = 2000;
unsigned int readingId = 0;
int sendSuccess = 0;
int sendFail = 0;
bool espNowReady = false;
bool emergency = false;
bool lastEmergency = false;
int rawGas = 0;
int calibratedGas = 0;

// Flame sensor debouncing
bool lastFlameState = false;
bool flameDetected = false;
unsigned long lastFlameChangeTime = 0;

// Buzzer variables
bool sirenActive = false;
bool freqRising = true;
int currentFreq = SIREN_FREQ_LOW;
unsigned long lastSirenUpdate = 0;

// WiFi reconnection
unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 30000;

DHT dht(DHTPIN, DHTTYPE);

// ========== Function Prototypes ==========
void initOLED();
void initESPNow();
void updateDisplay();
void showEmergency();
void startSiren();
void stopSiren();
void updateSiren();
void playBeep(int freq, int duration);
void readSensors();
void sendData();
void checkWiFiAndESPNow();

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║     SAFETY MONITOR - SECTION " + String(SECTION_ID) + "       ║");
  Serial.println("║     ESP-NOW CHANNEL 5                  ║");
  Serial.println("╚════════════════════════════════════════╝");
  
  // Initialize pins
  pinMode(STATUS_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(MQ2_PIN, INPUT);
  pinMode(FLAME_PIN, INPUT_PULLUP);
  digitalWrite(STATUS_LED, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  
  // Initialize sensors
  dht.begin();
  delay(500);
  
  // Initialize OLED
  initOLED();
  
  // Initialize WiFi in STA mode
  WiFi.mode(WIFI_STA);
  delay(100);
  
  // Print MAC address
  Serial.print("📡 Device MAC: ");
  Serial.println(WiFi.macAddress());
  
  // Set Wi-Fi channel to 5 (must match master)
  esp_wifi_set_channel(5, WIFI_SECOND_CHAN_NONE);
  WiFi.setChannel(5);
  delay(200);
  Serial.print("📡 Wi-Fi Channel set to: ");
  Serial.println(WiFi.channel());
  
  // Calibrate MQ-2
  Serial.println("\n🔧 Calibrating MQ-2 sensor...");
  int sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(MQ2_PIN);
    delay(200);
    Serial.print(".");
  }
  mq2Baseline = sum / 10;
  Serial.println();
  Serial.printf("✅ MQ-2 Baseline: %d\n", mq2Baseline);
  
  // Initialize ESP-NOW
  initESPNow();
  
  // Startup beep
  playBeep(1000, 100);
  delay(100);
  playBeep(1200, 100);
  
  Serial.println("\n✅ SYSTEM READY");
  Serial.println("========================================\n");
}

// ========== Main Loop ==========
void loop() {
  unsigned long now = millis();
  
  // Update siren
  updateSiren();
  
  // Check WiFi/ESP-NOW periodically
  if (now - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
    checkWiFiAndESPNow();
    lastWiFiCheck = now;
  }
  
  // Read and send data every 2 seconds
  if (espNowReady && (now - lastSendTime >= SEND_INTERVAL)) {
    readSensors();
    sendData();
    lastSendTime = now;
    
    // Update display
    updateDisplay();
    
    // Print status every 10 readings
    if (readingId % 10 == 0) {
      Serial.printf("📊 [S%d] T:%.1f°C G:%d F:%s | OK:%d ERR:%d\n",
                   SECTION_ID, 
                   sensorData.temperature, 
                   sensorData.gasLevel,
                   sensorData.fireDetected ? "YES" : "NO",
                   sendSuccess, 
                   sendFail);
    }
  }
  
  delay(10);
}

// ========== Read Sensors ==========
void readSensors() {
  // Read DHT11 with error handling
  float temp = dht.readTemperature();
  if (isnan(temp)) {
    temp = 25.0;
    Serial.println("⚠️ DHT read failed, using default temp");
  }
  sensorData.temperature = temp;
  
  // Read MQ-2 and convert to 0-800 scale
  rawGas = analogRead(MQ2_PIN);
  if (rawGas < mq2Baseline) rawGas = mq2Baseline;
  
  // Map from baseline-4095 to 0-800
  calibratedGas = map(rawGas, mq2Baseline, 4095, 0, MQ2_MAX_VALUE);
  if (calibratedGas > MQ2_MAX_VALUE) calibratedGas = MQ2_MAX_VALUE;
  if (calibratedGas < 0) calibratedGas = 0;
  sensorData.gasLevel = calibratedGas;
  
  // Read Flame Sensor with debouncing
  bool currentFlameState = (digitalRead(FLAME_PIN) == LOW);
  unsigned long now = millis();
  
  if (currentFlameState != lastFlameState) {
    lastFlameChangeTime = now;
  }
  
  if ((now - lastFlameChangeTime) > 500) {
    flameDetected = currentFlameState;
  }
  
  lastFlameState = currentFlameState;
  sensorData.fireDetected = flameDetected;
  
  // Check Emergency with hysteresis
  bool newEmergency = (sensorData.temperature > TEMP_THRESHOLD) ||
                      (sensorData.gasLevel > GAS_THRESHOLD) ||
                      sensorData.fireDetected;
  
  if (newEmergency != emergency) {
    if (newEmergency) {
      emergency = true;
    } else {
      static int safeCount = 0;
      if (safeCount >= 2) {
        emergency = false;
        safeCount = 0;
      } else {
        safeCount++;
      }
    }
  }
  
  // Handle siren
  if (emergency && !lastEmergency) {
    startSiren();
    Serial.println("\n🚨 EMERGENCY DETECTED!");
    if (sensorData.temperature > TEMP_THRESHOLD)
      Serial.printf("   🔥 Cause: High Temperature (%.1f°C)\n", sensorData.temperature);
    if (sensorData.gasLevel > GAS_THRESHOLD)
      Serial.printf("   💨 Cause: High Gas Level (%d ppm)\n", sensorData.gasLevel);
    if (sensorData.fireDetected)
      Serial.println("   🔥 Cause: Fire Detected");
  } else if (!emergency && lastEmergency) {
    stopSiren();
    Serial.println("✅ Emergency cleared\n");
  }
  lastEmergency = emergency;
  
  sensorData.sectionId = SECTION_ID;
  sensorData.readingId = readingId++;
}

// ========== Send Data via ESP-NOW ==========
void sendData() {
  if (!espNowReady) {
    Serial.println("⚠️ ESP-NOW not ready, skipping send");
    return;
  }
  
  esp_err_t result = esp_now_send(masterMac, (uint8_t*)&sensorData, sizeof(sensorData));
  
  if (result == ESP_OK) {
    sendSuccess++;
    digitalWrite(STATUS_LED, HIGH);
    delay(5);
    digitalWrite(STATUS_LED, LOW);
  } else {
    sendFail++;
    Serial.printf("❌ Send failed: %s\n", esp_err_to_name(result));
  }
}

// ========== ESP-NOW Functions (NO CALLBACK VERSION) ==========
void initESPNow() {
  Serial.println("\n🔧 Initializing ESP-NOW...");
  
  // Ensure Wi-Fi is in STA mode
  WiFi.mode(WIFI_STA);
  delay(100);
  
  // Set channel to 5
  esp_wifi_set_channel(5, WIFI_SECOND_CHAN_NONE);
  WiFi.setChannel(5);
  delay(100);
  
  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init failed!");
    espNowReady = false;
    return;
  }
  
  // IMPORTANT: Do NOT register send callback to avoid compilation error
  // The send status can be checked via the return value of esp_now_send()
  
  // Add peer
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, masterMac, 6);
  peerInfo.channel = 5;           // Force channel 5
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("❌ Failed to add peer");
    espNowReady = false;
    return;
  }
  
  espNowReady = true;
  Serial.println("✅ ESP-NOW Ready");
  Serial.printf("📡 Peer MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                masterMac[0], masterMac[1], masterMac[2],
                masterMac[3], masterMac[4], masterMac[5]);
  Serial.printf("📡 Channel: %d\n", WiFi.channel());
}

// Check and fix WiFi/ESP-NOW
void checkWiFiAndESPNow() {
  if (!espNowReady) {
    Serial.println("🔄 Attempting to re-init ESP-NOW...");
    initESPNow();
  }
  
  // Verify channel is still 5
  if (WiFi.channel() != 5) {
    Serial.println("⚠️ Channel changed! Resetting to channel 5...");
    esp_wifi_set_channel(5, WIFI_SECOND_CHAN_NONE);
    WiFi.setChannel(5);
  }
}

// ========== Siren Functions ==========
void startSiren() {
  sirenActive = true;
  freqRising = true;
  currentFreq = SIREN_FREQ_LOW;
  lastSirenUpdate = millis();
  Serial.println("🔊 Siren ACTIVATED");
}

void stopSiren() {
  sirenActive = false;
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, LOW);
  Serial.println("🔇 Siren deactivated");
}

void updateSiren() {
  if (!sirenActive) return;
  
  unsigned long now = millis();
  
  if (now - lastSirenUpdate >= SIREN_SPEED) {
    if (freqRising) {
      currentFreq += 30;
      if (currentFreq >= SIREN_FREQ_HIGH) {
        currentFreq = SIREN_FREQ_HIGH;
        freqRising = false;
      }
    } else {
      currentFreq -= 30;
      if (currentFreq <= SIREN_FREQ_LOW) {
        currentFreq = SIREN_FREQ_LOW;
        freqRising = true;
      }
    }
    
    tone(BUZZER_PIN, currentFreq);
    lastSirenUpdate = now;
  }
}

void playBeep(int freq, int duration) {
  tone(BUZZER_PIN, freq, duration);
  delay(duration);
  noTone(BUZZER_PIN);
}

// ========== OLED Display Functions ==========
void initOLED() {
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("⚠️ OLED not found");
    return;
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Initializing...");
  display.display();
  delay(1000);
  Serial.println("✅ OLED Ready");
}

void updateDisplay() {
  if (emergency) {
    showEmergency();
    return;
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // Header
  display.setCursor(0, 0);
  display.print("Section ");
  display.print(SECTION_ID);
  if (sendSuccess > 0) display.fillCircle(125, 4, 2, SSD1306_WHITE);
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  
  // Temperature
  display.setCursor(0, 15);
  display.print("Temp: ");
  display.print(sensorData.temperature, 1);
  display.print(" C");
  
  if (sensorData.temperature > 45) {
    display.fillCircle(115, 18, 4, SSD1306_WHITE);
  } else if (sensorData.temperature > 35) {
    display.drawCircle(115, 18, 4, SSD1306_WHITE);
  }
  
  // Gas Level with bar graph
  display.setCursor(0, 32);
  display.print("Gas: ");
  display.print(sensorData.gasLevel);
  display.print(" ppm");
  
  int barWidth = map(sensorData.gasLevel, 0, MQ2_MAX_VALUE, 0, 80);
  if (barWidth > 80) barWidth = 80;
  display.fillRect(45, 42, barWidth, 6, SSD1306_WHITE);
  display.drawRect(45, 42, 80, 6, SSD1306_WHITE);
  
  if (sensorData.gasLevel > GAS_THRESHOLD) {
    display.fillCircle(120, 45, 3, SSD1306_WHITE);
  }
  
  // Fire Status
  display.setCursor(0, 55);
  display.print("Fire: ");
  if (sensorData.fireDetected) {
    display.print("DETECTED!");
    static int flameX = 100;
    display.fillCircle(flameX, 58, 3, SSD1306_WHITE);
    flameX = (flameX == 100) ? 105 : 100;
  } else {
    display.print("SAFE");
  }
  
  display.display();
}

void showEmergency() {
  display.clearDisplay();
  static bool flash = false;
  static unsigned long lastFlash = 0;
  
  if (millis() - lastFlash > 300) {
    flash = !flash;
    lastFlash = millis();
  }
  
  if (flash) {
    display.fillRect(0, 0, 128, 64, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  
  display.setTextSize(2);
  display.setCursor(15, 15);
  display.println("EMERGENCY!");
  
  display.setTextSize(1);
  display.setCursor(25, 45);
  if (sensorData.temperature > TEMP_THRESHOLD) {
    display.print("HIGH TEMP!");
  } else if (sensorData.gasLevel > GAS_THRESHOLD) {
    display.print("HIGH GAS!");
  } else if (sensorData.fireDetected) {
    display.print("FIRE!");
  }
  
  // Blinking dots
  display.fillCircle(115, 58, 2, flash ? SSD1306_BLACK : SSD1306_WHITE);
  display.fillCircle(120, 58, 2, flash ? SSD1306_BLACK : SSD1306_WHITE);
  display.fillCircle(125, 58, 2, flash ? SSD1306_BLACK : SSD1306_WHITE);
  display.display();
}