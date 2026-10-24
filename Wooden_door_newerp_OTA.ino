#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <rdm6300.h>
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include <WiFiClientSecure.h>
#include <Update.h> // Required for OTA
#include <PubSubClient.h>

#define MQTT_TOPIC_LEN   128
#define MQTT_PAYLOAD_LEN 512
#define MQTT_MAX_PACKET_SIZE 1024 

// ================= OTA CONFIGURATION (NEW) =================
const char* api_secret = "put your api secret here"; /*" "*/
const char* machine_slug = "door-lock"; 

// SAFETY ID: 2 = Solenoid. This prevents it from installing "Wood" updates.
// The code will REJECT the update if the server says the type is not "2".
const char* MY_MACHINE_TYPE_ID = "1"; 
// REPLACE THIS with the real URL provided by your server admin
const char* otaApiUrl = "put your OTA API URL here";
// ===========================================================
// --- OTA VERSION ---
const char* currentFirmwareVersion = "1.0.4"; // Increment this when you upload new bin

volatile bool ota_request = false,skiploop = false; // Flag to trigger OTA
// ---------- Pins ----------
#define RDM6300_RX_PIN 4
#define READ_LED_PIN 18
#define GREN_LED_PIN 19
#define BUZZER_PIN 5
#define LOCK_PIN 13
const int AIN1 = 25;
const int AIN2 = 33;
const int PWMA = 32;
const int STBY = 26;
const int ST_LED = 2;
// ---------- One-time offline restart flag (survives ESP.restart) ----------
RTC_DATA_ATTR bool rtc_offlineRestarted = false;
// ---------- Globals ----------
WiFiManager wm;
Rdm6300 rdm6300;
volatile bool led_ST = false;          // true == connected
String Chipid = String(ESP.getEfuseMac(), DEC);
String ssid = "SDL-" + Chipid;
String read_data = "";
const char* File_Name = "/p.txt";
const char* MQTT_File = "/mqtt.txt";
// Reconnect policy (hourly batches)
const uint8_t  MAX_ATTEMPTS_PER_BATCH = 10;
const uint8_t  MAX_FAILED_BATCHES_BEFORE_RESTART = 5;
const unsigned long ATTEMPT_SPACING_MS = 2000;
const unsigned long BATCH_COOLDOWN_MS = 3600000UL;
const unsigned long CONNECT_TRY_MS     = 10000;
// Reconnect state
volatile uint8_t batchAttempts = 0;
volatile uint8_t failedBatches = 0;
unsigned long batchStartMs = 0;
unsigned long lastAttemptMs = 0;
bool batchActive = false;
// Cross-core flags / states
volatile bool whatspotMode = false;
volatile bool close_req = false;
volatile bool open_req = false;
volatile bool closeTimeStore = false;
volatile bool BUZZER_STATE = false;
volatile bool alarady_close = false;
volatile bool alarady_open = false;
volatile bool run_DenyTone = false;
volatile bool load_succes = false;
// NEW: schedule NetWorker immediately when needed
volatile bool request_sync_now = false;
// Timers (TRUE 30 min)
const unsigned long delayTime   = 30UL * 60UL * 1000UL; // 30 minutes periodic
const unsigned long delayData   = 30000;                // 30s first-load retry
unsigned long lastEarlySync = 0;
unsigned long lastPeriodicSync = 0;
// Door/tones timing
volatile unsigned long puncTime = 0;
volatile unsigned long openTime = 0;
volatile unsigned long closeTime = 0;
volatile unsigned long BUZZER_DELAY = 0;
volatile unsigned long BUZZER_FAULT_S_DELAY = 0;
volatile unsigned long BUZZER_FAULT_BLINK_DELAY = 0;

// Master list (BOUND FIX)
String m_cardArray[14] = {
  "0005254657","0005132457","0005119029","0005134642","0005128435","0005118248","0000166918",
  "0005128383","0005133906","0000252720","0005147289","0005132417","0005147839","0005136998"
};
const size_t M_CARD_COUNT = sizeof(m_cardArray)/sizeof(m_cardArray[0]);
// ---------- Net scheduling & backoff (like glass door) ----------
TaskHandle_t core0Handle = NULL;
TaskHandle_t core1Handle = NULL;
TaskHandle_t netTaskHandle = NULL;
volatile bool netTaskBusy = false;
unsigned long nextSyncDueMs = 0;
const unsigned long BACKOFF_MIN = 60000;   // 1 min
const unsigned long BACKOFF_MAX = 900000;  // 15 min
unsigned long backoffMs = BACKOFF_MIN;
bool inBackoff = false;
// NEW: restart trigger after deny tone if offline unknown card
volatile bool restartPending = false,FS_Faulty_update_requied = false;
// ........................................................... MQTT Setup.......................................................................

QueueHandle_t mqttQueue;

typedef struct {
  char topic[MQTT_TOPIC_LEN];
  char payload[MQTT_PAYLOAD_LEN];
  bool retained;
} mqtt_msg_t;

unsigned long mqtttryTime = 0;
unsigned long mqtttryConnectDelay = 30000;
// ====== MQTT Broker ====== IP "1....133" is only for SH8
const char* mqttServer = "103.92.218.74"; /*" private: ..., public : ..... ;*/
const int mqttPort = 1883;

// MQTT Username/Password
const char* mqttUser = "put your MQTT username here";
const char* mqttPassword = "put your MQTT password here";
String infoTopic = "devices/" + Chipid + "/fs";
String eventTopic = "devices/" + Chipid + "/event";
String statusTopic = "devices/" + Chipid + "/status";
String globalUpdateTopic = "ota/update";

// MQTT Client
WiFiClient espClient;
PubSubClient client(espClient);

// Unique Client ID using ESP32 MAC address
// String clientId;

// Device status topic

// .........................................................................MQTT Setup End..............................................................

// ---------- Forward declarations ----------
void core0Task(void *parameter);
void core1Task(void *parameter);
void NetWorker(void *parameter);
void reconnectWiFi();
void updateWifiLed();
// Net helpers (run inside NetWorker)

void cardCK(const String& card_number);

// Motor logic (UNCHANGED)
void open();
void close();
void stop();
void opening();
void closing();

// ====== Callback ======
bool mqttPublishQueue(const char* topic, const char* payload, bool retained) {
  if (mqttQueue == NULL) return false;

  mqtt_msg_t msg;
  mqtt_msg_t dump;   

  memset(&msg, 0, sizeof(msg));

  strncpy(msg.topic, topic, MQTT_TOPIC_LEN - 1);
  strncpy(msg.payload, payload, MQTT_PAYLOAD_LEN - 1);
  msg.retained = retained;

  // Try to send
  if (xQueueSend(mqttQueue, &msg, 0) != pdTRUE) {
    // Queue full → drop oldest
    xQueueReceive(mqttQueue, &dump, 0);
    // Retry send
    if (xQueueSend(mqttQueue, &msg, 0) != pdTRUE) {
      return false; // still failed
    }
  }

  return true;
}

void update(const String& msg) {
  if (mqttPublishQueue(eventTopic.c_str(), msg.c_str(), false)) {
    Serial.println("[MQTT] Feedback update queued: " + eventTopic);
  } else {
    Serial.println("[MQTT] Feedback update FAILED: " + eventTopic);
  }
}

void handleGet() {
  File file = LittleFS.open(MQTT_File, "r");
  if (file) {
      // Read entire file as a single string
      String fileContent = file.readString();
      // Send file content over MQTT
      update(fileContent);
      file.close();
      Serial.println("[FS] File content sent over MQTT.");
      return;
      
  } else {
      Serial.println("[FS] Card file missing.");
      bool isOk = mqttPublishQueue(eventTopic.c_str(), "Failed to open file for read", true);
      if (isOk) {
          Serial.println("File open failed, reported to: " + eventTopic);
      } else {
          Serial.println("File open report FAILED: " + eventTopic);
      }
  }
  file.close();
}

void handleReset() {
  Serial.println("[CMD] RESET");
  update("[CMD] RESET");
  wm.resetSettings();
  vTaskDelay(100 / portTICK_PERIOD_MS);
  ESP.restart();
}

void handleRun() {
  Serial.println("[CMD] RUN");
  update("[CMD] RUN");
  // motorRun();
}

void handleStop() {
  Serial.println("[CMD] STOP");
  update("[CMD] STOP");
  // motorStop();
}

void handleOpen() {
  puncTime = millis();
  open_req = true;
  run_DenyTone = false;
  update("[ACCESS] Allowed (MQTT CMD). Opening...");
  Serial.println("[ACCESS] Allowed (MQTT CMD). Opening...");
}

void handleSingleCard(JsonDocument &doc) {
  if (!doc["val"]) {
    Serial.println("[CMD] card: val missing");
    update("[CMD] card: val missing");
    return;
  }

  String card = doc["val"].as<String>();
  card.trim();
  Serial.print("[CMD] Single card: ");
  Serial.println(card);

  File file = LittleFS.open(MQTT_File, "w");
  if (file) {
    file.println(card);
    Serial.println("LittleFS Stored Single Carde.");
    update("LittleFS Stored Single Carde.");
  } else {
    update("LittleFS Failed to open for write.");
    Serial.println("[MQTT] LittleFS Failed to open for write: " + eventTopic);
  }
  file.close();
}

void handleCards(JsonDocument &doc) {
  if (!doc["val"].is<JsonArray>()) {
    Serial.println("[CMD] cards: val is not array");
    return;
  }
  JsonArray cardNumbers = doc["val"].as<JsonArray>();
  File file = LittleFS.open(MQTT_File, "w");
  if (file) {
    for (JsonVariant v : cardNumbers) {
      String s = v.as<String>(); s.trim();
      file.println(s);
    }
    
    load_succes = true;
    update("LittleFS Stored All Cardes.");
    // Serial.printf("[MQTT] Stored %u card(s) to %s\n", (unsigned)cardNumbers.size(), File_Name);
  } else {
    Serial.println("[FS] Failed to open file for write.");
    update("LittleFS Failed to open for write.");
    Serial.println("[MQTT] LittleFS fault reported to topic: " + eventTopic);
  }
  file.close();
}
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Convert the incoming char* topic to a String so we can compare it
  String currentTopic = String(topic); 
  Serial.print("[MQTT] Topic: ");
  Serial.println(currentTopic);

  String jsonStr;
  for (unsigned int i = 0; i < length; i++) {
    jsonStr += (char)payload[i];
  }

  // --- 1. Handle Global Broadcast Topic ---
  if (currentTopic == globalUpdateTopic) {
    Serial.println("[GLOBAL CMD] Broadcast update received!");
    update("Broadcast OTA check triggered.");
    ota_request = true;
    return; // Exit early
  }

  // --- 2. Handle Individual Device JSON Commands ---
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, jsonStr);

  if (error) {
    Serial.print("[MQTT] JSON Error: ");
    Serial.println(error.c_str());
    return;
  }

  const char* cmd = doc["cmd"];
  if (!cmd) return;

  String command = String(cmd);
  command.toLowerCase();
  if (command == "cards") {
    handleCards(doc);
  }
  else if (command == "card") {
    handleSingleCard(doc);
  }
  else if (command == "open") {
    handleOpen();
  }
  else if (command == "alt") {
    skiploop = doc["state"] | false; // default fallback {"cmd":"alt","state":true} || {"cmd":"alt","state":true}
    if (skiploop) {
      Serial.println("True");
    } else {
      Serial.println("False");
    }
    update("skiploop is now: " + String(skiploop));
  }
  else if (command == "update") {
    update("Checking for OTA Update...");
    ota_request = true; 
  }
  else if (command == "reset") {
    handleReset();
  }
  else if (command == "get") {
    handleGet();
  }
  else if (command == "restart") {
    update("Restarting....");
    ESP.restart();
  }
  else if (command == "format") {
    if (LittleFS.format()) {
      update("LittleFS formatted.");
    } else {
      update("LittleFS Format Failed.");
    }
  }
  else {
    Serial.print("[MQTT] Unknown cmd: ");
    Serial.println(command);
  }
}
void connectMQTT() {
  unsigned long now = millis();
  if(now - mqtttryTime >= mqtttryConnectDelay){
    mqtttryTime = now;
    if (!client.connected()) {

      Serial.print("Connecting to MQTT... ");
      if (client.connect(Chipid.c_str(), mqttUser, mqttPassword,
                        statusTopic.c_str(), 1, true, "offline")) {
        Serial.println("connected");

        client.publish(statusTopic.c_str(), "online", true);

        // Subscribe to unique device commands
        String cmdTopic = "devices/" + Chipid + "/cmd";
        client.subscribe(cmdTopic.c_str());

        // --- FIXED: Subscribe to global update topic ---
        client.subscribe(globalUpdateTopic.c_str()); 
        Serial.print("Subscribed to global topic: ");
        Serial.println(globalUpdateTopic);

        if(FS_Faulty_update_requied){
          FS_Faulty_update_requied = false;
          client.publish(infoTopic.c_str(), "LittleFS Faulty", true);
        }else{
          client.publish(infoTopic.c_str(), "LittleFS Ok", true);
        }
      } else {
        Serial.print("failed, rc=");
        Serial.print(client.state());
        Serial.println(" retrying in 30 seconds");
      }
    }
  }
}

// --------- HTTPS helpers (like glass door) ---------
static const char* kHost = "iot.neways3.com";

bool dns_ok() {
  IPAddress ip;
  int r = WiFi.hostByName(kHost, ip);
  if (r != 1) {
    Serial.println("[NET] DNS lookup failed; skipping HTTP this cycle.");
    return false;
  }
  return true;
}

int post_json_https(const char* url, const String& postData, String* out) {
  WiFiClientSecure client;
  client.setInsecure(); // ⚠️ For testing only, use proper cert in production
  client.setTimeout(3000);

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  http.setReuse(false);

  String currentUrl = String(url);
  int code = -1;

  // Follow up to 3 redirects
  for (int redirectCount = 0; redirectCount < 3; redirectCount++) {
    if (!http.begin(client, currentUrl)) {
      Serial.println("[HTTP] begin() failed");
      return -1;
    }
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    //http.addHeader("Content-Type", "application/json");
    //http.addHeader("Authorization", "101a1a587f0624b6b30cf78f579f54b5b30862267b99e760a");

    code = http.POST(postData);

    if (code > 0) {
      // Check for redirect codes
      if (code == HTTP_CODE_FOUND || code == HTTP_CODE_MOVED_PERMANENTLY ||
          code == HTTP_CODE_SEE_OTHER || code == HTTP_CODE_TEMPORARY_REDIRECT ||
          code == HTTP_CODE_PERMANENT_REDIRECT) {
        
        String newLocation = http.getLocation();
        Serial.printf("[HTTP] Redirect (%d) to: %s\n", code, newLocation.c_str());
        http.end();
        currentUrl = newLocation;
        continue; // retry with new URL
      }

      // Success
      if (out) {
        if (code == HTTP_CODE_OK) {
          *out = http.getString();
        } else {
          out->remove(0);
        }
      }
      http.end();
      client.stop();
      return code;
    } else {
      Serial.printf("[HTTP] POST failed: %s\n", http.errorToString(code).c_str());
      http.end();
      client.stop();
      return code;
    }
  }

  client.stop();
  return code; // return last code if too many redirects
}

// --------- Safe WDT kick (only for watched tasks) ---------
inline void wdt_kick_safe() {
  TaskHandle_t me = xTaskGetCurrentTaskHandle();
  if (me == core0Handle || me == core1Handle) {
    esp_task_wdt_reset();
  }
}

// --------- Backoff scheduling ----------
void scheduleAfterSuccess() {
  inBackoff = false;
  backoffMs = BACKOFF_MIN;
  // Online again → allow one-time offline restart later
  rtc_offlineRestarted = false;
  nextSyncDueMs = millis() + (load_succes ? delayTime : delayData);
}

void scheduleAfterFailure() {
  inBackoff = true;
  nextSyncDueMs = millis() + backoffMs;
  if (backoffMs < BACKOFF_MAX) backoffMs = min(backoffMs * 2, BACKOFF_MAX);
  Serial.printf("[HTTP] offline/backoff for %lus\n", (nextSyncDueMs - millis())/1000);
}
// ================= START OF NEW OTA LOGIC =================
void executeOTA(String url) {
  WiFiClientSecure* client = new WiFiClientSecure;
  client->setInsecure();
  client->setTimeout(120000); // 2 Minute Timeout
  
  HTTPClient http;
  http.setReuse(true); // Keep connection alive

  Serial.println("[OTA] Downloading from: " + url);

  if (http.begin(*client, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      int contentLength = http.getSize();
      Serial.printf("[OTA] Firmware Size: %d bytes\n", contentLength);

      if (contentLength > 0) {
        if (Update.begin(contentLength)) {
          Serial.println(F("[OTA] Writing to Flash..."));
          
          // Manual Loop
          WiFiClient* stream = http.getStreamPtr();
          uint8_t buff[1280]; 
          size_t written = 0;
          unsigned long lastByteTime = millis();

          while (http.connected() && written < contentLength) {
            size_t size = stream->available();
            if (size) {
              int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
              Update.write(buff, c);
              written += c;
              lastByteTime = millis(); 
              esp_task_wdt_reset(); // Keep watchdog happy
            } else {
              if (millis() - lastByteTime > 30000) { // 30s stall limit
                 Serial.println("[OTA] Error: Connection Stalled.");
                 break;
              }
              delay(1);
            }
          }

          if (written == contentLength) {
            if (Update.end()) {
              Serial.println(F("✅ OTA Success! Rebooting..."));
              update("OTA Success. Rebooting...");
              delay(1000);
              ESP.restart();
            } else {
               Serial.printf("[OTA] Error: %d\n", Update.getError());
               update("OTA Error: " + String(Update.getError()));
            }
          } else {
             Serial.printf("[OTA] Short write: %d/%d\n", written, contentLength);
             update("OTA Failed: Download Incomplete");
          }
        } else {
          Serial.println(F("[OTA] Not enough space"));
        }
      } else {
        Serial.println(F("[OTA] Empty content length"));
      }
    } else {
      Serial.printf("[OTA] Download failed: %d\n", httpCode);
    }
    http.end();
  }
  delete client;
}
void checkAndPerformOTA() {
  if (!led_ST) return;
  Serial.println(F("[OTA] Connecting to API..."));
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String postData = "api_secret=" + String(api_secret) + 
                    "&machine_slug=" + String(machine_slug) + 
                    "&device_code=" + String(Chipid) + 
                    "&machine_type_id=" + String(MY_MACHINE_TYPE_ID);

  if (http.begin(client, otaApiUrl)) {
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    int httpCode = http.POST(postData);

    Serial.print("[OTA] HTTP Code: "); 
    Serial.println(httpCode);

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.print("[OTA] Response: "); Serial.println(payload);

      DynamicJsonDocument doc(4096);
      DeserializationError error = deserializeJson(doc, payload);

      if (!error && doc["status"] == "success") {
        
        if (doc.containsKey("firmware") && !doc["firmware"].isNull()) {
            
            String serverTypeId = doc["firmware"]["machine_type_id"].as<String>();
            String serverVersion = doc["firmware"]["version_number"].as<String>();
            String downloadUrl = doc["file_url"].as<String>();
            String typeName = doc["firmware"]["machine_type_name"].as<String>();

            Serial.printf("[OTA] Server proposes: %s (Type ID: %s)\n", typeName.c_str(), serverTypeId.c_str());

            // Check if IDs match (String comparison)
            if (serverTypeId == String(MY_MACHINE_TYPE_ID)) {
              Serial.print(F("[OTA] Type Match. Checking version... "));
              if (serverVersion != currentFirmwareVersion && downloadUrl.length() > 0) {
                 Serial.printf("New version %s found. Updating...\n", serverVersion.c_str());
                 update("Downloading version " + serverVersion);
                 executeOTA(downloadUrl);
              } else {
                 Serial.println(F("Up to date."));
                 update("Already up to date.");
              }
            } else {
              Serial.printf("[OTA] REJECTED. Server sent Type %s, I am Type %s\n", serverTypeId.c_str(), MY_MACHINE_TYPE_ID);
              update("Update rejected: Wrong Device Type");
            }
        } else {
            Serial.println(F("[OTA] Success, but no firmware object."));
        }

      } else {
        Serial.println(F("[OTA] JSON Error or invalid status"));
      }
    } else {
      Serial.printf("[OTA] HTTP Error: %d\n", httpCode);
    }
    http.end();
  }
  ota_request = false;
}
// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println("\n[SETUP] Starting device...");
  Serial.print(F("ChipId: ")); Serial.println(Chipid);
  Serial.print(F("MAC: "));    Serial.println(WiFi.macAddress());

  pinMode(ST_LED, OUTPUT);
  pinMode(READ_LED_PIN, OUTPUT);
  pinMode(GREN_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(PWMA, OUTPUT);
  pinMode(STBY, OUTPUT);
  digitalWrite(STBY, HIGH);

  digitalWrite(READ_LED_PIN, LOW);
  digitalWrite(ST_LED, LOW);

  // LittleFS
  Serial.print("File System Mounting");
  int i = 0;
  if (!LittleFS.begin()) {
    Serial.println("An error has occurred while mounting LittleFS");
    delay(100);
    Serial.print(".");
    for (i = 0; i <= 100; i++) {
      if (!LittleFS.begin()) {
        if (i == 100) {
          // if (LittleFS.format()) {
          //   Serial.println("Filesystem formatted successfully. Attempting to mount again...");
          //   delay(10);
          //   if (LittleFS.begin()) {
          //     FS_Faulty_update_requied = false;
          //     Serial.println(F("File system opened LittleFS after formatting"));
          //   } else {
          //     ESP.restart();
          //   }
          // } else {
          //   
          //   ESP.restart();
          // }
          FS_Faulty_update_requied = true;
        }
      } else {
        return;
      }
      delay(20);
    }
  }
  if(FS_Faulty_update_requied){
    Serial.println("Filesystem not mounted");
  }else{
    Serial.print("Filesystem mounted after :");
    Serial.println(i);
  }
  // RFID
  rdm6300.begin(RDM6300_RX_PIN);
  delay(10);
  Serial.println("\nPlace RFID tag near the rdm6300...");

  // Setup MQTT server and callback
  client.setBufferSize(10240);   // 4 KB buffer
  client.setServer(mqttServer, mqttPort);
  client.setCallback(mqttCallback);
  mqttQueue = xQueueCreate(20, sizeof(mqtt_msg_t));
  if (!mqttQueue) {
    Serial.println("MQTT Queue create failed!");
    while (1);
  }


  // Watchdog (clean re-init; only core0/core1 are watched)
  esp_task_wdt_deinit();
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms     = 60000,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  esp_task_wdt_init(&twdt_config);

  // WiFiManager non-blocking portal
  Serial.println("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  wm.setDebugOutput(true);
  wm.setConnectTimeout(5);
  wm.setConfigPortalBlocking(false);

  unsigned long startTime = millis();
  if (wm.autoConnect(ssid.c_str())) {
    Serial.println(F("Wifi autoConnect!"));
  } else {
    Serial.println(F("Configportal running"));
  }

  // Give up to 10s for first connection
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
    delay(200);
    updateWifiLed();
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected at boot.");
    led_ST = true;
    digitalWrite(ST_LED, HIGH);
    // Online -> allow single offline restart later
    rtc_offlineRestarted = false;
    nextSyncDueMs = millis() + 2000; 
    ota_request = true;
    // quick first sync
  } else {
    Serial.println("\n[WIFI] Failed to connect at boot.");
    led_ST = false;
    digitalWrite(ST_LED, LOW);
    if (!wm.getConfigPortalActive()) {
      Serial.println("[WM] Starting config portal (SDL-...)");
      wm.startConfigPortal(ssid.c_str());
    }
    nextSyncDueMs = millis() + 5000; // try soon even if offline
  }

  // Create tasks pinned to cores
  BaseType_t t0 = xTaskCreatePinnedToCore(core0Task, "Core0Task", 8192,  NULL, 1, &core0Handle, 0);
  BaseType_t t1 = xTaskCreatePinnedToCore(core1Task, "Core1Task", 10000, NULL, 1, &core1Handle, 1);

  if (t0 == pdPASS && t1 == pdPASS) {
    esp_task_wdt_add(core0Handle);
    esp_task_wdt_add(core1Handle);
    Serial.println("[TASK] Both tasks started.");
  } else {
    Serial.println("[TASK] Task creation failed! Restarting...");
    delay(500);
    ESP.restart();
  }
}
void loop() {
  // Serial.printf("[Arduino loop ] Running on core: %d\n", xPortGetCoreID());
  // vTaskDelay(10 / portTICK_PERIOD_MS);
}
// ================== CORE 0: orchestrator (WiFi/HTTP scheduling) ==================
void core0Task(void *parameter) {
  mqtt_msg_t msg; 
  Serial.printf("[CORE 0] Running on core: %d\n", xPortGetCoreID());
  for (;;) {

    wdt_kick_safe();
    wm.process();
    reconnectWiFi();

     // --- ✅ OTA LOGIC ---
    if (ota_request && led_ST) { 
      Serial.println(F("[OTA] Request received. Connecting to API..."));
      checkAndPerformOTA();
    }

    // ----------------------

    updateWifiLed();
    if (!client.connected() && led_ST) {
      connectMQTT();
    }else{
      // Limit publish per cycle
      int sent = 0;
      while (xQueueReceive(mqttQueue, &msg, 0) == pdTRUE && sent < 5) {
        Serial.print("[MQTT] Topic   : ");
        Serial.println(msg.topic);

        Serial.print("[MQTT] Payload : ");
        Serial.println(msg.payload);

        Serial.print("[MQTT] Retain  : ");
        Serial.println(msg.retained);

        client.publish(msg.topic, msg.payload, msg.retained);
        Serial.println("[MQTT] Event published\n");
        sent++;
      }
    }
    client.loop();

    unsigned long now = millis();

    // Early sync (first-load retries) -> nudge when not in backoff
    if (led_ST && !load_succes && (now - lastEarlySync >= delayData)) {
      lastEarlySync = now;
      if (!inBackoff) nextSyncDueMs = millis();
    }

    // Periodic sync every 10 minutes -> respect backoff
    if (led_ST && (now - lastPeriodicSync >= delayTime)) {
      lastPeriodicSync = now;
      if (!inBackoff) nextSyncDueMs = millis();
    }

    // Immediate sync requested (e.g., from cardCK while online)
    if (led_ST && request_sync_now) {
      request_sync_now = false;
      if (!inBackoff) nextSyncDueMs = millis();
    }

    // Start NetWorker only when due and not already running
    if (led_ST && !netTaskBusy && netTaskHandle == NULL && millis() >= nextSyncDueMs) {
      netTaskBusy = true;
      xTaskCreatePinnedToCore(NetWorker, "NetWorker", 8192, NULL, 1, &netTaskHandle, 0);
    }

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}
// ================== CORE 1: RFID/Motor ==================
void core1Task(void *parameter) {
  Serial.printf("[CORE 1] Running on core: %d\n", xPortGetCoreID());
  for (;;) {
    wdt_kick_safe();
    if(skiploop){
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }
    // RFID read (non-blocking)
    if (rdm6300.get_new_tag_id()) {
      String cardString = String(rdm6300.get_tag_id(), DEC);
      while (cardString.length() < 10) cardString = "0" + cardString;
      read_data = cardString;
      Serial.print("[RFID] Read: "); Serial.println(read_data);
      cardCK(read_data);
      digitalWrite(READ_LED_PIN, rdm6300.get_tag_id());
    }

    // Door state machine (UNCHANGED)
    if (open_req && !close_req) {
      opening();
    }

    if (close_req) {
      unsigned long now = millis();
      if (now > openTime + 5000) {
        if (!closeTimeStore) {
          closeTime = now;
          closeTimeStore = true;
        }
        closing();
      }
    }

    // Deny tone handler + optional one-time restart (like glass door)
    if (run_DenyTone) {
      unsigned long now = millis();
      if (now >= BUZZER_FAULT_S_DELAY + 300) {
        run_DenyTone = false;
        digitalWrite(BUZZER_PIN, LOW);
        digitalWrite(READ_LED_PIN, LOW);

        if (restartPending) {
          restartPending = false;
          rtc_offlineRestarted = true; // remember we already restarted this offline period
          Serial.println("[SYS] Offline unknown card -> single auto-restart.");
          // delay(50);
          vTaskDelay(50 / portTICK_PERIOD_MS);
          ESP.restart();
        }
      }
      if (now >= BUZZER_FAULT_BLINK_DELAY + 80) {
        BUZZER_FAULT_BLINK_DELAY = now;
        BUZZER_STATE = !BUZZER_STATE;
        digitalWrite(BUZZER_PIN, BUZZER_STATE);
        digitalWrite(READ_LED_PIN, BUZZER_STATE);
      }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}
// ================== NetWorker (HTTPS + backoff; not WDT-watched) ==================
bool syncCards(const String& response, const char* File_Name) {
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    Serial.printf("[HTTP] JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray serverCards = doc["card_numbers"].as<JsonArray>();

  // Load existing cards from file
  std::vector<String> storedCards;
  if (LittleFS.exists(File_Name)) {
    File file = LittleFS.open(File_Name, "r");
    if (file) {
      while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) storedCards.push_back(line);
      }
      file.close();
    } else {
      file.close();
      Serial.println("[FS] Failed to open file for read, formatting...");
      if (!LittleFS.format()) {
        LittleFS.format();
        ESP.restart();
      }
    }
  } else {
    Serial.println("[FS] File not found, formatting...");
    if (!LittleFS.format()) {
      vTaskDelay(100);
      ESP.restart();
    }else{
      File newFile = LittleFS.open("/p.txt", "w");
      if (newFile) {
        newFile.println("first line of data");
        newFile.close();
        Serial.println("File created and written successfully!");
      } else {
        Serial.println("Failed to create/open file.");
        ESP.restart();
      }
    }
  }

  // Convert server cards to vector
  std::vector<String> serverCardsVec;
  for (JsonVariant v : serverCards) {
    String s = v.as<String>(); s.trim();
    if (s.length() > 0) serverCardsVec.push_back(s);
  }

  // Compare sets manually
  bool changed = false;

  // Add new cards
  for (auto& s : serverCardsVec) {
    bool found = false;
    for (auto& c : storedCards) {
      if (c == s) { found = true; break; }
    }
    if (!found) {
      Serial.printf("[SYNC] Adding new card: %s\n", s.c_str());
      storedCards.push_back(s);
      changed = true;
    }
  }

  // Remove cards not in server
  for (auto it = storedCards.begin(); it != storedCards.end();) {
    bool found = false;
    for (auto& s : serverCardsVec) {
      if (*it == s) { found = true; break; }
    }
    if (!found) {
      Serial.printf("[SYNC] Removing card: %s\n", it->c_str());
      it = storedCards.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }

  // Write back if changed OR file was missing
  if (changed || !LittleFS.exists(File_Name)) {
    File file = LittleFS.open(File_Name, "w");
    if (file) {
      for (auto& s : storedCards) {
        file.println(s);
      }
      file.close();

      // ✅ Verify by re-reading
      File verify = LittleFS.open(File_Name, "r");
      if (verify) {
        Serial.println("[SYNC] Verifying stored cards:");
        while (verify.available()) {
          String line = verify.readStringUntil('\n');
          line.trim();
          if (line.length() > 0) {
            Serial.println("  " + line);
          }
        }
        verify.close();
      }

      // ✅ Notify system
      update("Card list updated");
      load_succes = true;
      Serial.printf("[SYNC] Stored %u card(s) to %s\n", (unsigned)storedCards.size(), File_Name);
      return true;
    } else {
      Serial.println("[FS] Failed to open file for write.");
      if (LittleFS.format()) {
        return syncCards(response, File_Name); // retry once
      } else {
        ESP.restart();
      }
    }
  } else {
    Serial.println("[SYNC] No changes, file unchanged.");
    return true; // still OK, nothing to update
  }

  return false; // fallback
}
void NetWorker(void *parameter) {
  Serial.printf("[Network Task running core] Running on core: %d\n", xPortGetCoreID());

  String postData = "api_secret=" + String(api_secret) +
                    "&device_code=" + String(Chipid);

  bool ok = false;

  // ---------------- MAIN IF -----------------
  if (WiFi.status() == WL_CONNECTED && dns_ok()) {

    // ----------- Device Status Update -----------
    Serial.println("[HTTP] NetWorker: deviceLog + fetch cards");

    int code = post_json_https(
                  "put your device status update URL here",
                  postData,
                  nullptr
                );

    Serial.printf("[HTTP] device status code: %d\n", code);

    ok |= (code == 200);
    vTaskDelay(1);


    // ----------- Fetch Cards -----------
    String response;
    code = post_json_https(
             "put your fetch cards URL here",
             postData,
             &response
           );

    Serial.printf("[HTTP] fetch cards code: %d\n", code);

    if (code == 200) {
      ok = syncCards(response, File_Name);
      load_succes = ok;
    } else {
      load_succes = false;
    }

    vTaskDelay(1);
  }else {// ---------------- ELSE BLOCK (correct) -----------------
    Serial.println("[HTTP] NetWorker: skip (offline/DNS)");
  }

  // ---------------- POST HTTP LOGIC -----------------
  if (ok){
    scheduleAfterSuccess();
  }else{
    // ----------- Device Status Update -----------
    Serial.println("[HTTP] NetWorker: deviceLog + fetch cards");
    int code = post_json_https(
                  "put your device status update URL here",
                  postData,
                  nullptr
                );

    Serial.printf("[HTTP] device status code: %d\n", code);

    ok |= (code == 200);
    vTaskDelay(1);


    // ----------- Fetch Cards -----------
    String response;
    code = post_json_https(
             "put your fetch cards URL here",
             postData,
             &response
           );

    Serial.printf("[HTTP] fetch cards code: %d\n", code);

    if (code == 200) {
      ok = syncCards(response, File_Name);
      load_succes = ok;
    } else {
      load_succes = false;
    }
    vTaskDelay(1);
    
    if(ok){
      scheduleAfterSuccess();
    }else{
      scheduleAfterFailure();
    }
  } 

  netTaskBusy = false;
  netTaskHandle = NULL;

  vTaskDelete(NULL);  // Finish task
}

// ================== Card check ==================
void cardCK(const String& card_number) {
  // 1) Check file
  File file = LittleFS.open(File_Name, "r");
  if (file) {
    while (file.available()) {
      String cardNumber = file.readStringUntil('\n');
      cardNumber.trim();
      if (card_number == cardNumber && cardNumber.length() > 0) {
        puncTime = millis();
        open_req = true;
        run_DenyTone = false;
        Serial.println("[ACCESS] Allowed (LittleFS). Opening...");
        file.close();
        return;
      }
    }
  } else {
    Serial.println("[FS] Card file missing.");
    bool isOk = mqttPublishQueue(eventTopic.c_str(),"Failed to open file for read",true);
    if(isOk){
      Serial.println("File open Failed, reported to : " + eventTopic);
    }else{
      Serial.println("File open reported Failed" + eventTopic);
    }
    
  }
  file.close();
  // 2) Check master
  for (size_t i = 0; i < M_CARD_COUNT; i++) {
    if (m_cardArray[i] == card_number) {
      puncTime = millis();
      open_req = true;
      run_DenyTone = false;
      Serial.println("[ACCESS] Allowed (Master). Opening...");
      return;
    }
  }

  // 3) Not found
  Serial.println("[ACCESS] Card not found in FS or Master list.");

  if (WiFi.status() != WL_CONNECTED) {
    // Arm one-time restart to run after deny tone finishes
    if (!rtc_offlineRestarted) {
      restartPending = true;
      Serial.println("[WIFI] Offline → will auto-restart once after buzzer.");
    } else {
      Serial.println("[WIFI] Offline but one-time restart already used this offline period.");
    }
  } else {
    // WiFi is up — schedule immediate sync via NetWorker
    delay(300);
    if (!inBackoff) nextSyncDueMs = millis();
    Serial.println("[HTTP] Scheduled immediate sync; present card again after sync.");
  }

  // Deny feedback (brief blink/buzz)
  run_DenyTone = true;
  BUZZER_FAULT_S_DELAY = millis();
  Serial.println("[ACCESS] Denied. Buzzing...");
}
// ================== Motor control (UNCHANGED) ==================
void open() {
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, HIGH);
  analogWrite(PWMA, 255);
}

void close() {
  digitalWrite(AIN1, HIGH);
  digitalWrite(AIN2, LOW);
  analogWrite(PWMA, 255);
}

void stop() {
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, LOW);
  analogWrite(PWMA, 0);
}

void opening() {
  unsigned long now = millis();
  if (now > puncTime + 60) { // a touch longer for reliable actuation
    stop();
    openTime = now;
    open_req = false;
    close_req = true;
    alarady_open = false;
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(GREN_LED_PIN, LOW);
    Serial.println("Door Open");
  } else if (!alarady_open) {
    open();
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(GREN_LED_PIN, HIGH);
    alarady_open = true;
    Serial.println("All Right Opening The Door");
  }
}

void closing() {
  unsigned long now = millis();
  if (now > closeTime + 60) {
    stop();
    open_req = false;
    close_req = false;
    closeTimeStore = false;
    alarady_close = false;
    Serial.println("Door Close");
  } else if (!alarady_close) {
    close();
    alarady_close = true;
  }
}

// ================== WiFi reconnect + LED ==================
void reconnectWiFi() {
  wl_status_t st = WiFi.status();

  if (st == WL_CONNECTED) {
    if (wm.getConfigPortalActive()) wm.stopConfigPortal();
    updateWifiLed();
    // Online -> allow next offline period to trigger one-time restart again
    rtc_offlineRestarted = false;

    batchActive   = false;
    batchAttempts = 0;
    failedBatches = 0;
    batchStartMs  = 0;
    lastAttemptMs = 0;
    return;
  }

  updateWifiLed();

  if (!wm.getConfigPortalActive()) {
    Serial.println("[WM] Disconnected -> starting config portal (SDL-...).");
    wm.startConfigPortal(ssid.c_str());
  }
  wm.process();

  // Pause STA while a user is in the portal so erase/save works cleanly
  int clients = WiFi.softAPgetStationNum();
  if (clients > 0) {
    esp_wifi_disconnect();
    WiFi.disconnect(false, false);
    batchActive   = false;
    batchAttempts = 0;
    lastAttemptMs = 0;
    return;
  }

  unsigned long now = millis();

  // Start a new batch when cooldown passes
  if (!batchActive) {
    if (batchStartMs == 0 || (now - batchStartMs >= BATCH_COOLDOWN_MS)) {
      batchActive   = true;
      batchAttempts = 0;
      batchStartMs  = now;
      Serial.printf("[WIFI] Starting reconnect batch (failed batches so far: %u)\n", failedBatches);
    } else {
      return;
    }
  }

  if (batchAttempts < MAX_ATTEMPTS_PER_BATCH) {
    if (now - lastAttemptMs < ATTEMPT_SPACING_MS) return;

    lastAttemptMs = now;
    batchAttempts++;
    Serial.printf("[WIFI] Batch attempt %u/%u...\n", batchAttempts, MAX_ATTEMPTS_PER_BATCH);

    esp_wifi_disconnect();
    WiFi.begin();
    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < CONNECT_TRY_MS) {
      wm.process();
      wdt_kick_safe();
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
      updateWifiLed();
      if (wm.getConfigPortalActive()) wm.stopConfigPortal();

      // Reconnected: reset backoff & schedule quick sync
      inBackoff = false;
      backoffMs = BACKOFF_MIN;
      rtc_offlineRestarted = false;
      nextSyncDueMs = millis() + 2000;
      ota_request = true;

      batchActive   = false;
      batchAttempts = 0;
      failedBatches = 0;
      batchStartMs  = 0;
      lastAttemptMs = 0;
      return;
    }

    Serial.println("[WIFI] Attempt failed.");
    return;
  }

  // Batch exhausted -> 1h cooldown (and eventual restart)
  batchActive = false;
  failedBatches++;
  lastAttemptMs = 0;
  Serial.printf("[WIFI] Batch failed (%u/%u). Cooling down 1 hour...\n",
                failedBatches, MAX_FAILED_BATCHES_BEFORE_RESTART);

  if (failedBatches >= MAX_FAILED_BATCHES_BEFORE_RESTART) {
    Serial.println("[WIFI] 5 failed hourly batches -> restarting device.");
    delay(200);
    ESP.restart();
  }
}

void updateWifiLed() {
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(ST_LED, HIGH);  // ON when connected
    led_ST = true;
  } else {
    digitalWrite(ST_LED, LOW);   // OFF when not connected
    led_ST = false;
  }
}