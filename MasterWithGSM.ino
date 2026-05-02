/*
 * MASTER CONTROL UNIT - FINAL PRODUCTION VERSION v6.0
 * FIXED: Immediate Fan and Buzzer activation on emergency
 */

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HardwareSerial.h>

// ========== WiFi Configuration ==========
const char* ssid = "Yadhronics";
const char* password = "Yadh#2902";

// ========== Pin Definitions ==========
#define FAN_RELAY 26      // Exhaust Fan - OFF in normal, ON in emergency
#define BUZZER_PIN 25     // Buzzer - OFF in normal, ACTIVE in emergency
#define GREEN_LED 33      // Normal status LED
#define RED_LED 32        // Emergency status LED
#define LOCK_RELAY 27     // Gate Lock - LOCKED in normal, UNLOCKED in emergency

// ========== Threshold Configuration ==========
#define TEMP_THRESHOLD 45.0
#define GAS_THRESHOLD 100

// ========== GSM Module Configuration ==========
#define GSM_RX 16
#define GSM_TX 17
HardwareSerial gsmSerial(2);

String phoneNumber = "+918681847551";

// GSM State variables
bool gsmInitialized = false;
bool networkRegistered = false;
bool systemReadySMSSent = false;
unsigned long lastGsmCheck = 0;
unsigned long lastAlertTime = 0;
const unsigned long ALERT_COOLDOWN = 60000;
const unsigned long GSM_HEARTBEAT = 30000;

// Call state
bool isCallActive = false;
unsigned long callStartTime = 0;
const unsigned long CALL_DURATION = 10000;

// GSM mutex
bool processingGSM = false;

// ========== Data Structure ==========
typedef struct struct_message {
  int sectionId;
  float temperature;
  int gasLevel;
  bool fireDetected;
  unsigned int readingId;
  char slaveMac[18];
} struct_message;

struct SectionData {
  float temperature;
  int gasLevel;
  bool fireDetected;
  bool emergency;
  unsigned long lastUpdate;
  bool dataReceived;
  unsigned int lastReadingId;
  String slaveMac;
  bool slaveConnected;
  unsigned long lastSeen;
};

SectionData sections[3];

// ========== System Variables ==========
WebServer server(80);
bool systemEmergency = false;
unsigned long lastActivityTime = 0;
bool inSleepMode = false;

bool buzzerState = false;
unsigned long lastBuzzerToggle = 0;
const unsigned long BUZZER_ON_TIME = 200;
unsigned long emergencyStartTime = 0;

unsigned int packetsReceived = 0;
uint8_t masterMac[6];
IPAddress localIP;

// Emergency tracking
struct EmergencyEvent {
  int sectionId;
  unsigned long timestamp;
  String triggerReason;
  float temp;
  int gas;
  bool fire;
  bool alertSent;
};

EmergencyEvent currentEmergency = {0, 0, "", 0, 0, false, false};

// Queue for ESP-NOW data during GSM ops
struct_message pendingData[10];
int pendingDataCount = 0;

// ========== Function Prototypes ==========
void initWiFi();
void initESPNow();
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len);
void processPendingData();
void processReceivedData(struct_message receivedData);
void checkEmergency();
void activateEmergency(int sectionId, String triggerReason);
void deactivateEmergency();
void handleEmergencyLED();
void handleNormalLED();
void controlActuators();
bool initGSM();
bool checkGSMConnection();
void sendSMS(String message);
void makeCall(String number);
String readGSMResponse(unsigned long timeout);
void checkGSMAndNetwork();
void checkAndSendSystemReadySMS();
void sendEmergencyAlert(int sectionId, String triggerReason);
void setupWebServer();
void handleRoot();
void handleData();
void printMACAddress();
void readMacAddress();
void checkSleepMode();
void clearGsmBuffer();

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n");
  Serial.println("╔══════════════════════════════════════════════╗");
  Serial.println("║     MASTER CONTROL UNIT - PRODUCTION v6.0   ║");
  Serial.println("║     IMMEDIATE EMERGENCY RESPONSE            ║");
  Serial.println("╚══════════════════════════════════════════════╝");
  
  // Initialize pins
  pinMode(FAN_RELAY, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(LOCK_RELAY, OUTPUT);
  
  // Set initial NORMAL state
  digitalWrite(FAN_RELAY, LOW);      // Fan OFF
  digitalWrite(BUZZER_PIN, LOW);     // Buzzer OFF
  digitalWrite(GREEN_LED, HIGH);     // Green LED ON
  digitalWrite(RED_LED, LOW);        // Red LED OFF
  digitalWrite(LOCK_RELAY, HIGH);    // Gate LOCKED
  
  // Initialize section data
  for (int i = 0; i < 3; i++) {
    sections[i].temperature = 0;
    sections[i].gasLevel = 0;
    sections[i].fireDetected = false;
    sections[i].emergency = false;
    sections[i].lastUpdate = 0;
    sections[i].dataReceived = false;
    sections[i].lastReadingId = 0;
    sections[i].slaveMac = "";
    sections[i].slaveConnected = false;
    sections[i].lastSeen = 0;
  }
  
  // Initialize WiFi
  initWiFi();
  
  // Print MAC address
  printMACAddress();
  
  // Initialize ESP-NOW
  initESPNow();
  
  // Setup Web Server
  setupWebServer();
  
  // Initialize GSM
  Serial.println("\n📱 Initializing GSM Module...");
  gsmSerial.begin(9600, SERIAL_8N1, GSM_RX, GSM_TX);
  delay(2000);
  
  processingGSM = true;
  for (int attempt = 1; attempt <= 5; attempt++) {
    Serial.printf("Attempt %d/5\n", attempt);
    if (initGSM()) {
      gsmInitialized = true;
      Serial.println("✅ GSM Ready!");
      break;
    }
    delay(2000);
  }
  processingGSM = false;
  processPendingData();
  
  if (gsmInitialized) {
    Serial.println("Checking network...");
    processingGSM = true;
    checkGSMAndNetwork();
    processingGSM = false;
    processPendingData();
  }
  
  Serial.println("\n✅ SYSTEM READY");
  Serial.println("================================================");
  Serial.println("🌐 WEB ACCESS: http://" + localIP.toString());
  Serial.println("📡 Gas Threshold: 100ppm");
  Serial.println("⚡ Emergency: IMMEDIATE Fan + Buzzer activation");
  Serial.println("================================================\n");
  
  lastActivityTime = millis();
  lastGsmCheck = millis();
}

void loop() {
  server.handleClient();
  
  if (!processingGSM) {
    processPendingData();
  }
  
  // Check slave connection status
  for (int i = 0; i < 3; i++) {
    if (sections[i].slaveConnected && (millis() - sections[i].lastSeen > 15000)) {
      sections[i].slaveConnected = false;
      sections[i].dataReceived = false;
      Serial.printf("⚠️ Section %c slave DISCONNECTED\n", 'A' + i);
    }
  }
  
  // Check for emergency - THIS WILL ACTIVATE ACTUATORS IMMEDIATELY
  checkEmergency();
  
  // Handle LED indicators
  if (systemEmergency) {
    handleEmergencyLED();
  } else {
    handleNormalLED();
  }
  
  // Handle active call - Auto hangup after duration
  if (isCallActive && (millis() - callStartTime > CALL_DURATION)) {
    processingGSM = true;
    gsmSerial.println("ATH");
    delay(500);
    processingGSM = false;
    isCallActive = false;
    Serial.println("📱 Call ended");
  }
  
  // Periodic GSM check
  if (gsmInitialized && (millis() - lastGsmCheck > GSM_HEARTBEAT) && !processingGSM) {
    processingGSM = true;
    checkGSMAndNetwork();
    processingGSM = false;
    lastGsmCheck = millis();
  }
  
  if (!systemEmergency) {
    checkSleepMode();
  }
  
  delay(10);
}

// ========== ESP-NOW with Queue ==========

void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
  struct_message receivedData;
  memcpy(&receivedData, incomingData, sizeof(receivedData));
  
  // Extract sender MAC
  char senderMac[18];
  snprintf(senderMac, sizeof(senderMac), "%02X:%02X:%02X:%02X:%02X:%02X",
           recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
           recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);
  strcpy(receivedData.slaveMac, senderMac);
  
  if (processingGSM && pendingDataCount < 10) {
    pendingData[pendingDataCount] = receivedData;
    pendingDataCount++;
    return;
  }
  
  processReceivedData(receivedData);
}

void processPendingData() {
  for (int i = 0; i < pendingDataCount; i++) {
    processReceivedData(pendingData[i]);
  }
  pendingDataCount = 0;
}

void processReceivedData(struct_message receivedData) {
  int sectionIndex = receivedData.sectionId - 1;
  
  if (sectionIndex >= 0 && sectionIndex < 3) {
    if (receivedData.readingId > sections[sectionIndex].lastReadingId) {
      sections[sectionIndex].temperature = receivedData.temperature;
      sections[sectionIndex].gasLevel = receivedData.gasLevel;
      sections[sectionIndex].fireDetected = receivedData.fireDetected;
      sections[sectionIndex].lastUpdate = millis();
      sections[sectionIndex].dataReceived = true;
      sections[sectionIndex].lastReadingId = receivedData.readingId;
      sections[sectionIndex].slaveMac = String(receivedData.slaveMac);
      sections[sectionIndex].slaveConnected = true;
      sections[sectionIndex].lastSeen = millis();
      packetsReceived++;
      
      Serial.printf("📥 [%c] Temp:%.1f°C Gas:%dppm Fire:%s MAC:%s\n",
                   'A' + sectionIndex,
                   receivedData.temperature,
                   receivedData.gasLevel,
                   receivedData.fireDetected ? "YES" : "NO",
                   receivedData.slaveMac);
      
      lastActivityTime = millis();
    }
  }
}

// ========== GSM Functions ==========

void clearGsmBuffer() {
  while(gsmSerial.available()) {
    gsmSerial.read();
  }
}

String readGSMResponse(unsigned long timeout) {
  String response = "";
  unsigned long start = millis();
  
  while (millis() - start < timeout) {
    while (gsmSerial.available()) {
      char c = gsmSerial.read();
      response += c;
    }
    delay(10);
  }
  return response;
}

bool initGSM() {
  clearGsmBuffer();
  
  for (int i = 0; i < 3; i++) {
    gsmSerial.println("AT");
    delay(1000);
    String response = readGSMResponse(2000);
    
    if (response.indexOf("OK") != -1) {
      Serial.println("   ✅ GSM responding");
      gsmSerial.println("ATE0");
      delay(500);
      readGSMResponse(1000);
      gsmSerial.println("AT+CMGF=1");
      delay(500);
      readGSMResponse(1000);
      return true;
    }
    delay(500);
  }
  Serial.println("   ❌ No response");
  return false;
}

bool checkGSMConnection() {
  clearGsmBuffer();
  gsmSerial.println("AT");
  delay(500);
  String response = readGSMResponse(1500);
  return (response.indexOf("OK") != -1);
}

void checkGSMAndNetwork() {
  if (!gsmInitialized) return;
  
  clearGsmBuffer();
  gsmSerial.println("AT+CREG?");
  delay(1500);
  String response = readGSMResponse(2000);
  
  if (response.indexOf("+CREG: 0,1") != -1 || response.indexOf("+CREG: 0,5") != -1) {
    if (!networkRegistered) {
      networkRegistered = true;
      Serial.println("   ✅ Network REGISTERED!");
      checkAndSendSystemReadySMS();
    }
  } else {
    networkRegistered = false;
  }
}

void makeCall(String number) {
  if (!gsmInitialized || !networkRegistered) {
    Serial.println("   ❌ Cannot make call - GSM not ready");
    return;
  }
  
  if (isCallActive) {
    Serial.println("   ⚠️ Call already active");
    return;
  }
  
  Serial.println("   📞 Making emergency call...");
  clearGsmBuffer();
  gsmSerial.print("ATD");
  gsmSerial.print(number);
  gsmSerial.println(";");
  delay(2000);
  isCallActive = true;
  callStartTime = millis();
  Serial.println("   📞 Call initiated - will ring for 10 seconds");
}

void sendSMS(String message) {
  if (!gsmInitialized || !networkRegistered) {
    Serial.println("   ❌ Cannot send SMS - GSM not ready");
    return;
  }
  
  Serial.println("   📱 Sending SMS...");
  clearGsmBuffer();
  
  gsmSerial.println("AT+CMGF=1");
  delay(500);
  readGSMResponse(1000);
  clearGsmBuffer();
  
  gsmSerial.print("AT+CMGS=\"");
  gsmSerial.print(phoneNumber);
  gsmSerial.println("\"");
  delay(2000);
  
  gsmSerial.print(message);
  delay(500);
  gsmSerial.write(26);
  delay(5000);
  
  String response = readGSMResponse(3000);
  if (response.indexOf("OK") != -1 || response.indexOf("+CMGS") != -1) {
    Serial.println("   ✅ SMS sent successfully!");
  } else {
    Serial.println("   ⚠️ SMS may not have sent");
  }
}

void checkAndSendSystemReadySMS() {
  if (!systemReadySMSSent && networkRegistered) {
    String message = "INDUSTRIAL SAFETY SYSTEM\nSystem: ONLINE\nNetwork: REGISTERED\nGas Threshold: 100ppm\nWeb: http://" + localIP.toString();
    sendSMS(message);
    systemReadySMSSent = true;
  }
}

void sendEmergencyAlert(int sectionId, String triggerReason) {
  char sectionName = 'A' + (sectionId - 1);
  float temp = sections[sectionId-1].temperature;
  int gas = sections[sectionId-1].gasLevel;
  bool fire = sections[sectionId-1].fireDetected;
  
  String message = "🚨 EMERGENCY ALERT 🚨\n";
  message += "==================\n";
  message += "Section: " + String(sectionName) + "\n";
  message += "Trigger: " + triggerReason + "\n";
  message += "Temperature: " + String(temp, 1) + "C\n";
  message += "Gas Level: " + String(gas) + " ppm\n";
  message += "Fire: " + String(fire ? "DETECTED" : "No") + "\n";
  message += "==================\n";
  message += "Actions Taken:\n";
  message += "- Exhaust Fan: ON\n";
  message += "- Alarm: ACTIVE\n";
  message += "- Gate: UNLOCKED (for evacuation)\n";
  message += "- Call: INITIATED\n";
  message += "- SMS: SENT\n";
  message += "==================\n";
  message += "Monitor: http://" + localIP.toString();
  
  Serial.println("\n🚨 EMERGENCY SEQUENCE STARTED 🚨");
  Serial.println("Trigger: " + triggerReason);
  Serial.println("Step 1: Making emergency call...");
  makeCall(phoneNumber);
  
  delay(3000);
  
  Serial.println("Step 2: Sending emergency SMS...");
  sendSMS(message);
  
  Serial.println("✅ Emergency alerts sent!");
}

// ========== EMERGENCY HANDLERS - FIXED FOR IMMEDIATE RESPONSE ==========

void checkEmergency() {
  bool previousEmergency = systemEmergency;
  
  for (int i = 0; i < 3; i++) {
    if (sections[i].dataReceived && sections[i].slaveConnected) {
      bool dataStale = (millis() - sections[i].lastUpdate) > 10000;
      
      if (!dataStale) {
        bool emergency = false;
        String triggerReason = "";
        
        // Check GAS first (threshold 100ppm)
        if (sections[i].gasLevel > GAS_THRESHOLD) {
          emergency = true;
          triggerReason = "GAS LEAK (" + String(sections[i].gasLevel) + "ppm)";
          Serial.printf("⚠️ GAS THRESHOLD EXCEEDED: %d ppm > %d ppm\n", sections[i].gasLevel, GAS_THRESHOLD);
        }
        // Check FIRE
        else if (sections[i].fireDetected) {
          emergency = true;
          triggerReason = "FIRE DETECTED";
          Serial.println("⚠️ FIRE DETECTED!");
        }
        // Check TEMPERATURE
        else if (sections[i].temperature > TEMP_THRESHOLD) {
          emergency = true;
          triggerReason = "HIGH TEMP (" + String(sections[i].temperature, 1) + "C)";
          Serial.printf("⚠️ HIGH TEMPERATURE: %.1fC > %.1fC\n", sections[i].temperature, TEMP_THRESHOLD);
        }
        
        sections[i].emergency = emergency;
        
        if (emergency && !systemEmergency) {
          systemEmergency = true;
          emergencyStartTime = millis();
          
          // IMMEDIATELY ACTIVATE ACTUATORS
          activateEmergency(i + 1, triggerReason);
          
          // Store emergency details
          currentEmergency.sectionId = i + 1;
          currentEmergency.timestamp = millis();
          currentEmergency.triggerReason = triggerReason;
          currentEmergency.temp = sections[i].temperature;
          currentEmergency.gas = sections[i].gasLevel;
          currentEmergency.fire = sections[i].fireDetected;
          currentEmergency.alertSent = false;
          
          Serial.printf("\n🔴🔴🔴 EMERGENCY IN SECTION %c! 🔴🔴🔴\n", 'A' + i);
          Serial.printf("   Reason: %s\n", triggerReason.c_str());
          Serial.printf("   Temp: %.1fC | Gas: %dppm | Fire: %s\n", 
                       sections[i].temperature, sections[i].gasLevel,
                       sections[i].fireDetected ? "YES" : "NO");
          Serial.println("⚡ IMMEDIATE ACTION: Fan ON, Buzzer ACTIVE, Gate UNLOCKED");
          
          // Send GSM alerts (Call + SMS) - this happens AFTER actuators are activated
          processingGSM = true;
          sendEmergencyAlert(i + 1, triggerReason);
          processingGSM = false;
        }
      }
    }
  }
  
  // Check if emergency should clear - all sections normal for 3 seconds
  if (systemEmergency) {
    bool anyEmergency = false;
    for (int i = 0; i < 3; i++) {
      if (sections[i].emergency) {
        anyEmergency = true;
        break;
      }
    }
    
    if (!anyEmergency && (millis() - emergencyStartTime > 3000)) {
      deactivateEmergency();
      systemEmergency = false;
      Serial.println("\n✅ Emergency cleared - System back to NORMAL");
      Serial.println("   Fan: OFF | Buzzer: OFF | Gate: LOCKED");
    }
  }
}

void activateEmergency(int sectionId, String triggerReason) {
  // IMMEDIATE actuator activation
  digitalWrite(FAN_RELAY, HIGH);     // Fan ON immediately
  digitalWrite(LOCK_RELAY, LOW);     // Gate UNLOCKED immediately
  
  // Buzzer will be handled in handleEmergencyLED() with pulsing
  // But start it immediately
  digitalWrite(BUZZER_PIN, HIGH);
  buzzerState = true;
  
  Serial.println("⚡ ACTUATORS ACTIVATED:");
  Serial.println("   🔄 Fan: ON");
  Serial.println("   🔊 Buzzer: ACTIVE");
  Serial.println("   🚪 Gate: UNLOCKED");
}

void deactivateEmergency() {
  // Return to normal state
  digitalWrite(FAN_RELAY, LOW);      // Fan OFF
  digitalWrite(BUZZER_PIN, LOW);     // Buzzer OFF
  digitalWrite(LOCK_RELAY, HIGH);    // Gate LOCKED
  buzzerState = false;
  
  Serial.println("⚡ ACTUATORS DEACTIVATED:");
  Serial.println("   🔄 Fan: OFF");
  Serial.println("   🔊 Buzzer: OFF");
  Serial.println("   🚪 Gate: LOCKED");
}

void handleEmergencyLED() {
  // Red LED blinking
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 500) {
    digitalWrite(RED_LED, !digitalRead(RED_LED));
    digitalWrite(GREEN_LED, LOW);
    lastBlink = millis();
  }
  
  // Buzzer pulsing
  if (millis() - lastBuzzerToggle >= BUZZER_ON_TIME) {
    buzzerState = !buzzerState;
    digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    lastBuzzerToggle = millis();
  }
}

void handleNormalLED() {
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(RED_LED, LOW);
  // Buzzer is already OFF from deactivateEmergency()
}

void controlActuators() {
  // Actuators are already controlled in activateEmergency/deactivateEmergency
  // This function is kept for compatibility but does nothing extra
}

// ========== WiFi Functions ==========

void initWiFi() {
  WiFi.mode(WIFI_STA);
  delay(100);
  
  Serial.print("\nConnecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi Connected");
    localIP = WiFi.localIP();
    Serial.print("📡 IP: ");
    Serial.println(localIP);
  } else {
    Serial.println("\n⚠️ WiFi connection failed");
    localIP = IPAddress(0, 0, 0, 0);
  }
}

void readMacAddress() {
  String mac = WiFi.macAddress();
  sscanf(mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
         &masterMac[0], &masterMac[1], &masterMac[2],
         &masterMac[3], &masterMac[4], &masterMac[5]);
}

void printMACAddress() {
  readMacAddress();
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║     MASTER ESP32 MAC ADDRESS          ║");
  Serial.print("║  ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", masterMac[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println("  ║");
  Serial.print("║  {");
  for (int i = 0; i < 6; i++) {
    Serial.printf("0x%02X", masterMac[i]);
    if (i < 5) Serial.print(", ");
  }
  Serial.println("}  ║");
  Serial.println("╚════════════════════════════════════════╝\n");
}

// ========== ESP-NOW ==========

void initESPNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init failed!");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);
  Serial.println("✅ ESP-NOW Ready");
}

// ========== Sleep Mode ==========

void checkSleepMode() {
  if (!systemEmergency && (millis() - lastActivityTime > 300000)) {
    if (!inSleepMode) {
      Serial.println("💤 Entering light sleep...");
      inSleepMode = true;
      esp_sleep_enable_timer_wakeup(60000000);
      esp_light_sleep_start();
      lastActivityTime = millis();
      Serial.println("⏰ Woke from sleep");
    }
  } else {
    inSleepMode = false;
  }
}

// ========== Web Server ==========

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.println("✅ Web Server Started");
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Industrial Safety System</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Arial, sans-serif;
            background: linear-gradient(135deg, #1a1a2e, #16213e);
            min-height: 100vh;
            padding: 20px;
            color: white;
        }
        .container { max-width: 1400px; margin: 0 auto; }
        .header {
            text-align: center;
            margin-bottom: 30px;
            padding: 20px;
            background: rgba(255,255,255,0.1);
            border-radius: 20px;
            backdrop-filter: blur(10px);
        }
        .header h1 { font-size: 2em; margin-bottom: 10px; }
        .threshold-info {
            font-size: 0.9em;
            margin-top: 10px;
            padding: 8px;
            background: rgba(0,0,0,0.3);
            border-radius: 10px;
            display: inline-block;
        }
        .status-badge {
            display: inline-block;
            padding: 10px 30px;
            border-radius: 50px;
            font-weight: bold;
            font-size: 1.2em;
            margin-top: 10px;
        }
        .status-safe { background: #00b894; }
        .status-emergency { 
            background: #d63031; 
            animation: pulse 1s infinite;
        }
        @keyframes pulse {
            0%, 100% { opacity: 1; transform: scale(1); }
            50% { opacity: 0.8; transform: scale(1.05); }
        }
        .stats-bar {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
            gap: 15px;
            margin-bottom: 30px;
        }
        .stat-card {
            background: rgba(255,255,255,0.1);
            padding: 15px;
            border-radius: 15px;
            text-align: center;
            backdrop-filter: blur(10px);
        }
        .stat-card .label { font-size: 0.8em; opacity: 0.8; }
        .stat-card .value { font-size: 1.3em; font-weight: bold; margin-top: 5px; }
        .sections-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(350px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }
        .card {
            background: white;
            border-radius: 20px;
            padding: 20px;
            color: #333;
            transition: transform 0.3s;
        }
        .card.emergency { 
            background: linear-gradient(135deg, #fff, #ffe0e0);
            border: 3px solid #d63031;
            animation: borderPulse 0.5s infinite;
        }
        @keyframes borderPulse {
            0%, 100% { border-color: #d63031; }
            50% { border-color: #ff7675; }
        }
        .card h2 { 
            margin-bottom: 15px; 
            padding-bottom: 10px; 
            border-bottom: 2px solid #dfe6e9;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        .slave-status {
            font-size: 0.7em;
            padding: 4px 8px;
            border-radius: 20px;
            font-weight: normal;
        }
        .slave-connected { background: #00b894; color: white; }
        .slave-disconnected { background: #d63031; color: white; }
        .sensor-reading {
            margin: 12px 0;
            padding: 10px;
            background: #f8f9fa;
            border-radius: 12px;
            display: flex;
            justify-content: space-between;
        }
        .sensor-label { font-weight: bold; color: #636e72; }
        .sensor-value { font-size: 1.2em; font-weight: bold; }
        .warning { color: #d63031; }
        .safe { color: #00b894; }
        .normal-actions {
            margin-top: 15px;
            padding: 12px;
            background: #e8f8f5;
            border-radius: 12px;
            border-left: 4px solid #00b894;
        }
        .normal-actions h4 { color: #00b894; margin-bottom: 8px; }
        .emergency-actions {
            margin-top: 15px;
            padding: 12px;
            background: #fff3cd;
            border-radius: 12px;
            border-left: 4px solid #d63031;
        }
        .emergency-actions h4 { color: #d63031; margin-bottom: 8px; }
        .action-item {
            display: flex;
            justify-content: space-between;
            margin: 5px 0;
            font-size: 0.9em;
        }
        .action-on { color: #d63031; font-weight: bold; }
        .action-off { color: #00b894; font-weight: bold; }
        .action-locked { color: #d63031; font-weight: bold; }
        .action-unlocked { color: #00b894; font-weight: bold; }
        .control-status {
            background: white;
            border-radius: 20px;
            padding: 20px;
            color: #333;
        }
        .control-status h3 { margin-bottom: 15px; color: #2d3436; }
        .control-item {
            display: flex;
            justify-content: space-between;
            margin: 10px 0;
            padding: 12px;
            background: #f8f9fa;
            border-radius: 12px;
        }
        .control-on { background: #d63031; color: white; padding: 5px 15px; border-radius: 20px; }
        .control-off { background: #00b894; color: white; padding: 5px 15px; border-radius: 20px; }
        .timestamp { text-align: center; margin-top: 20px; font-size: 0.8em; opacity: 0.7; }
        @media (max-width: 768px) {
            .sections-grid { grid-template-columns: 1fr; }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🏭 INDUSTRIAL SAFETY SYSTEM</h1>
            <div class="threshold-info">🌡️ Temp Threshold: 45°C | 💨 Gas Threshold: 100ppm | ⚡ Immediate Response</div>
            <div id="systemStatus" class="status-badge status-safe">✅ SYSTEM NORMAL</div>
        </div>
        <div class="stats-bar">
            <div class="stat-card"><div class="label">📊 PACKETS</div><div class="value" id="packets">0</div></div>
            <div class="stat-card"><div class="label">📱 GSM</div><div class="value" id="gsmStatus">--</div></div>
            <div class="stat-card"><div class="label">📡 NETWORK</div><div class="value" id="networkStatus">--</div></div>
            <div class="stat-card"><div class="label">🔗 ACTIVE SLAVES</div><div class="value" id="activeSlaves">0</div></div>
        </div>
        <div class="sections-grid" id="sectionsGrid"></div>
        <div class="control-status">
            <h3>⚙️ SYSTEM CONTROL STATUS</h3>
            <div id="controlStatus"></div>
        </div>
        <div class="timestamp" id="timestamp">--</div>
    </div>
    <script>
        function fetchData() {
            fetch('/data').then(r=>r.json()).then(d=>updateDashboard(d)).catch(e=>console.error(e));
        }
        
        function updateDashboard(d) {
            document.getElementById('packets').textContent = d.packetsReceived;
            document.getElementById('gsmStatus').innerHTML = d.gsmReady ? '✅ READY' : '⚠️ OFF';
            document.getElementById('networkStatus').innerHTML = d.networkRegistered ? '✅ OK' : '⚠️ NO';
            
            let activeCount = 0;
            const grid = document.getElementById('sectionsGrid');
            grid.innerHTML = '';
            
            for(let i=0;i<d.sections.length;i++){
                const s = d.sections[i];
                if(s.slaveConnected) activeCount++;
                
                const card = document.createElement('div');
                card.className = 'card' + (s.emergency ? ' emergency' : '');
                
                const slaveStatusClass = s.slaveConnected ? 'slave-connected' : 'slave-disconnected';
                const slaveStatusText = s.slaveConnected ? '🟢 CONNECTED' : '🔴 DISCONNECTED';
                const slaveMacDisplay = s.slaveConnected ? s.slaveMac.substring(0, 17) : '--';
                
                let stateActionsHtml = '';
                if (s.emergency) {
                    let triggerText = '';
                    if (s.gasLevel > d.gasThreshold) triggerText = '💨 GAS LEAK';
                    else if (s.fireDetected) triggerText = '🔥 FIRE';
                    else if (s.temperature > d.tempThreshold) triggerText = '🌡️ HIGH TEMP';
                    
                    stateActionsHtml = `
                        <div class="emergency-actions">
                            <h4>🚨 EMERGENCY STATE - ACTIONS TAKEN</h4>
                            <div class="action-item"><span>🔄 Exhaust Fan:</span><span class="action-on">ON (Immediate)</span></div>
                            <div class="action-item"><span>🔊 Alarm Buzzer:</span><span class="action-on">ACTIVE (Immediate)</span></div>
                            <div class="action-item"><span>🚪 Gate Lock:</span><span class="action-unlocked">UNLOCKED (Evacuation)</span></div>
                            <div class="action-item"><span>📞 Emergency Call:</span><span class="action-on">INITIATED</span></div>
                            <div class="action-item"><span>📱 Alert SMS:</span><span class="action-on">SENT</span></div>
                            <div class="action-item" style="margin-top:8px;"><span>⚠️ Trigger:</span><span class="action-on">${triggerText}</span></div>
                        </div>
                    `;
                } else if (s.slaveConnected) {
                    stateActionsHtml = `
                        <div class="normal-actions">
                            <h4>✅ NORMAL STATE</h4>
                            <div class="action-item"><span>🔄 Exhaust Fan:</span><span class="action-off">OFF</span></div>
                            <div class="action-item"><span>🔊 Alarm Buzzer:</span><span class="action-off">OFF</span></div>
                            <div class="action-item"><span>🚪 Gate Lock:</span><span class="action-locked">LOCKED (Secure)</span></div>
                            <div class="action-item"><span>📡 Monitoring:</span><span class="action-off">ACTIVE</span></div>
                        </div>
                    `;
                } else {
                    stateActionsHtml = `
                        <div class="normal-actions">
                            <h4>⚠️ NO CONNECTION</h4>
                            <div class="action-item"><span>🔄 Exhaust Fan:</span><span class="action-off">OFF</span></div>
                            <div class="action-item"><span>🔊 Alarm Buzzer:</span><span class="action-off">OFF</span></div>
                            <div class="action-item"><span>🚪 Gate Lock:</span><span class="action-locked">LOCKED</span></div>
                            <div class="action-item"><span>📡 Status:</span><span class="action-on">Waiting for Slave</span></div>
                        </div>
                    `;
                }
                
                card.innerHTML = `
                    <h2>
                        📍 SECTION ${String.fromCharCode(65+i)}
                        <span class="slave-status ${slaveStatusClass}">${slaveStatusText}</span>
                    </h2>
                    <div class="sensor-reading">
                        <span class="sensor-label">🌡️ TEMPERATURE</span>
                        <span class="sensor-value ${s.dataReceived && s.temperature > d.tempThreshold ? 'warning' : 'safe'}">${s.dataReceived ? s.temperature.toFixed(1)+'°C' : '--'}</span>
                    </div>
                    <div class="sensor-reading">
                        <span class="sensor-label">💨 GAS LEVEL</span>
                        <span class="sensor-value ${s.dataReceived && s.gasLevel > d.gasThreshold ? 'warning' : 'safe'}">${s.dataReceived ? s.gasLevel+' ppm' : '--'}</span>
                    </div>
                    <div class="sensor-reading">
                        <span class="sensor-label">🔥 FIRE STATUS</span>
                        <span class="sensor-value ${s.fireDetected ? 'warning' : 'safe'}">${s.dataReceived ? (s.fireDetected ? '⚠️ DETECTED' : '✓ SAFE') : '--'}</span>
                    </div>
                    <div class="sensor-reading">
                        <span class="sensor-label">📡 SLAVE MAC</span>
                        <span class="sensor-value" style="font-size:0.9em;">${slaveMacDisplay}</span>
                    </div>
                    <div class="sensor-reading">
                        <span class="sensor-label">📊 SYSTEM STATUS</span>
                        <span class="sensor-value ${s.emergency ? 'warning' : 'safe'}">${!s.dataReceived ? 'NO SIGNAL' : (s.emergency ? '🚨 EMERGENCY' : '✅ NORMAL')}</span>
                    </div>
                    ${stateActionsHtml}
                `;
                grid.appendChild(card);
            }
            
            document.getElementById('activeSlaves').textContent = activeCount;
            
            const isEmergency = d.systemEmergency;
            document.getElementById('controlStatus').innerHTML = `
                <div class="control-item"><span>🔄 EXHAUST FAN</span><span class="${d.fanStatus ? 'control-on' : 'control-off'}">${d.fanStatus ? 'RUNNING' : 'OFF'}</span></div>
                <div class="control-item"><span>🔊 ALARM BUZZER</span><span class="${d.buzzerStatus ? 'control-on' : 'control-off'}">${d.buzzerStatus ? 'ACTIVE' : 'OFF'}</span></div>
                <div class="control-item"><span>🚪 GATE LOCK</span><span class="${d.lockStatus ? 'control-on' : 'control-off'}">${d.lockStatus ? 'LOCKED' : 'UNLOCKED'}</span></div>
                <div class="control-item" style="margin-top:10px; background:${isEmergency ? '#fff3cd' : '#e8f8f5'}">
                    <span>📌 SYSTEM MODE</span>
                    <span style="font-weight:bold; color:${isEmergency ? '#d63031' : '#00b894'}">${isEmergency ? '🚨 EMERGENCY MODE' : '✅ NORMAL MODE'}</span>
                </div>
            `;
            
            document.getElementById('systemStatus').className = 'status-badge ' + (d.systemEmergency ? 'status-emergency' : 'status-safe');
            document.getElementById('systemStatus').innerHTML = d.systemEmergency ? '🚨 EMERGENCY ACTIVE! 🚨' : '✅ SYSTEM NORMAL';
            document.getElementById('timestamp').textContent = 'Last Update: ' + new Date().toLocaleTimeString();
        }
        
        setInterval(fetchData, 1000);
        fetchData();
    </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleData() {
  String json = "{";
  json += "\"sections\":[";
  for (int i = 0; i < 3; i++) {
    json += "{";
    json += "\"temperature\":" + String(sections[i].temperature) + ",";
    json += "\"gasLevel\":" + String(sections[i].gasLevel) + ",";
    json += "\"fireDetected\":" + String(sections[i].fireDetected ? "true" : "false") + ",";
    json += "\"emergency\":" + String(sections[i].emergency ? "true" : "false") + ",";
    json += "\"dataReceived\":" + String(sections[i].dataReceived ? "true" : "false") + ",";
    json += "\"slaveConnected\":" + String(sections[i].slaveConnected ? "true" : "false") + ",";
    json += "\"slaveMac\":\"" + sections[i].slaveMac + "\"";
    json += "}";
    if (i < 2) json += ",";
  }
  json += "],";
  json += "\"systemEmergency\":" + String(systemEmergency ? "true" : "false") + ",";
  json += "\"fanStatus\":" + String(digitalRead(FAN_RELAY) == HIGH ? "true" : "false") + ",";
  json += "\"lockStatus\":" + String(digitalRead(LOCK_RELAY) == HIGH ? "true" : "false") + ",";
  json += "\"buzzerStatus\":" + String(digitalRead(BUZZER_PIN) == HIGH ? "true" : "false") + ",";
  json += "\"tempThreshold\":" + String(TEMP_THRESHOLD) + ",";
  json += "\"gasThreshold\":" + String(GAS_THRESHOLD) + ",";
  json += "\"packetsReceived\":" + String(packetsReceived) + ",";
  json += "\"gsmReady\":" + String(gsmInitialized ? "true" : "false") + ",";
  json += "\"networkRegistered\":" + String(networkRegistered ? "true" : "false") + ",";
  json += "\"timestamp\":\"" + String(millis() / 1000) + "s\"";
  json += "}";
  server.send(200, "application/json", json);
}