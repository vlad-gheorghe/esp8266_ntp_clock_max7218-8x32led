// ===============================================
// ESP8266 MAX7219 NTP Clock - Ported Version
// Features: WiFi recovery, EEPROM config, auto-geolocation
// Original source: https://github.com/stechiez/esp32-c3-max7217-ntp
// ===============================================

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <EEPROM.h>
#include <time.h>
#include <MD_Parola.h>  
#include <SPI.h>
#include <ArduinoJson.h>

#include "Font_Data.h"
#include "WiFiConfig.h"

// =============== HARDWARE CONFIGURATION ===============
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4

// Display SPI pins (Standard ESP8266 / NodeMCU pinout)
// D7 = GPIO13 (MOSI / DATA)
// D5 = GPIO14 (CLK / SCK)
// D8 = GPIO15 (CS / SS)
#define DATA_PIN  13
#define CLK_PIN   14 
#define CS_PIN    15 

MD_Parola P = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// =============== DISPLAY CONFIGURATION ===============
#define SPEED_TIME  75
#define PAUSE_TIME  0
#define MAX_MESG    20

// =============== TIME CONFIGURATION ===============
int32_t TIMEZONE_SECONDS = 0;
const int DST = 0;

// =============== LIGHT SENSOR CONFIGURATION ===============
#define LIGHT_SENSOR_PIN A0
#define LIGHT_CHECK_INTERVAL 2000 // Verifică lumina la fiecare 2 secunde

// Valori citite de ADC (A0). Ajustează-le dacă este nevoie după teste:
// Pe ESP8266 (NodeMCU/Wemos) analogRead aduce valori între 0 și 1023.
#define ADC_MIN 100   // Prag pentru întuneric
#define ADC_MAX 800   // Prag pentru lumină puternică

// Nivelurile de luminozitate MAX7219 (0 - 15)
#define BRIGHTNESS_MIN 0   // Luminozitate noapte/întuneric (fără să deranjeze)
#define BRIGHTNESS_MAX 10  // Luminozitate zi (poți pune până la 15)

// NTP servers for time synchronization
const char* NTP_SERVERS[] = {"pool.ntp.org", "time.nist.gov"};

// Geolocation API endpoint
const char* GEOLOCATION_API = "http://ip-api.com/json/?fields=country,city,lat,lon,timezone,offset";

// =============== EEPROM CONFIGURATION ===============
#define EEPROM_SIZE 512
#define SSID_ADDR 0
#define PASS_ADDR 33
#define INIT_FLAG_ADDR 66
#define TIMEZONE_ADDR 70
#define LOCATION_ADDR 80

// =============== WIFI CONFIGURATION ===============
#define WIFI_RETRY_DELAY 500       // ms between connection attempts
#define WIFI_MAX_RETRIES 20        // max attempts during setup
#define WIFI_RECONNECT_INTERVAL 30000  // check connection every 30s
#define WIFI_RECONNECT_TIMEOUT 10000   // timeout for reconnection attempt

// =============== GLOBAL VARIABLES ===============
// Time variables
uint16_t h, m, s;
uint8_t dow, day, month;
String year;

// Display buffers
char szTime[9];      // HH:MM\0
char szsecond[4];    // SS\0

// WiFi state tracking
uint32_t lastWiFiCheck = 0;
bool wifiConnected = false;

// Geolocation data
struct {
  char country[32];
  char city[32];
  float latitude;
  float longitude;
  char timezone[40];
  int32_t utcOffset;  // UTC offset in seconds
} location;

// Configuration storage
struct {
  char ssid[32];
  char password[65];
} wifiConfig;

// Forward declarations & EEPROM Helper functions for ESP8266
void debugPrint(const char* msg);
void saveWiFiConfig();
void printLocationInfo();

void writeStringToEEPROM(int addr, const char* str) {
  int len = strlen(str);
  for (int i = 0; i < len; i++) {
    EEPROM.write(addr + i, str[i]);
  }
  EEPROM.write(addr + len, '\0');
}

void readStringFromEEPROM(int addr, char* buf, int maxLen) {
  int i = 0;
  while (i < maxLen - 1) {
    char c = EEPROM.read(addr + i);
    buf[i] = c;
    if (c == '\0') break;
    i++;
  }
  buf[i] = '\0';
}

// =============== EEPROM MANAGEMENT ===============
void initEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  
  uint8_t initFlag = EEPROM.read(INIT_FLAG_ADDR);
  if (initFlag == 0xAA) {
    // Load previously saved credentials
    readStringFromEEPROM(SSID_ADDR, wifiConfig.ssid, sizeof(wifiConfig.ssid));
    readStringFromEEPROM(PASS_ADDR, wifiConfig.password, sizeof(wifiConfig.password));
    
    // Validate loaded credentials - if SSID is empty, reset to defaults
    if (strlen(wifiConfig.ssid) == 0) {
      debugPrint("EEPROM credentials corrupted! Resetting to defaults...");
      strcpy(wifiConfig.ssid, DEFAULT_SSID);
      strcpy(wifiConfig.password, DEFAULT_PASSWORD);
      saveWiFiConfig();
    } else {
      debugPrint("Loaded WiFi credentials from EEPROM");
    }
  } else {
    // First run - use default credentials from WiFiConfig.h
    strcpy(wifiConfig.ssid, DEFAULT_SSID);
    strcpy(wifiConfig.password, DEFAULT_PASSWORD);
    saveWiFiConfig();
    debugPrint("Using default WiFi credentials");
  }
}

void saveWiFiConfig() {
  writeStringToEEPROM(SSID_ADDR, wifiConfig.ssid);
  writeStringToEEPROM(PASS_ADDR, wifiConfig.password);
  EEPROM.write(INIT_FLAG_ADDR, 0xAA);
  EEPROM.commit();
}

// =============== DEBUG UTILITIES ===============
void debugPrint(const char* msg) {
  Serial.println(msg);
}

// =============== GEOLOCATION & TIMEZONE ===============
bool fetchGeolocation() {
  debugPrint("Fetching geolocation and timezone...");
  
  WiFiClient client;
  HTTPClient http;
  
  http.setTimeout(5000);
  if (!http.begin(client, GEOLOCATION_API)) {
    debugPrint("Unable to connect to Geolocation API");
    return false;
  }
  
  int httpCode = http.GET();
  
  if (httpCode != HTTP_CODE_OK) {
    debugPrint("Geolocation API request failed");
    http.end();
    return false;
  }
  
  String payload = http.getString();
  http.end();
  
  Serial.print("Geolocation response: ");
  Serial.println(payload);
  
  // Parse JSON response
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    debugPrint("JSON parsing failed");
    return false;
  }
  
  // Extract location data
  strlcpy(location.country, doc["country"] | "Unknown", sizeof(location.country));
  strlcpy(location.city, doc["city"] | "Unknown", sizeof(location.city));
  location.latitude = doc["lat"] | 0.0f;
  location.longitude = doc["lon"] | 0.0f;
  strlcpy(location.timezone, doc["timezone"] | "UTC", sizeof(location.timezone));
  location.utcOffset = doc["offset"] | 0;

  Serial.print("Raw timezone offset from API: ");
  Serial.println(location.utcOffset);

  if (abs(location.utcOffset) < 1000) {
    Serial.print("Interpreting offset as hours, converting to seconds: ");
    Serial.println(location.utcOffset);
    location.utcOffset = location.utcOffset * 3600;
    Serial.print("Normalized timezone offset (seconds): ");
    Serial.println(location.utcOffset);
  }

  TIMEZONE_SECONDS = location.utcOffset;
  printLocationInfo();
  
  return true;
}

void printLocationInfo() {
  debugPrint("=== Geolocation Information ===");
  Serial.print("Location: ");
  Serial.print(location.city[0] ? location.city : "Unknown");
  Serial.print(", ");
  Serial.print(location.country[0] ? location.country : "Unknown");
  Serial.print(" (");
  Serial.print(location.latitude, 4);
  Serial.print(", ");
  Serial.print(location.longitude, 4);
  Serial.print(")\t");

  float hours = location.utcOffset / 3600.0f;
  Serial.print("Timezone: ");
  Serial.print(location.timezone[0] ? location.timezone : "UTC");
  Serial.print(" (UTC");
  Serial.print(hours, 2);
  Serial.println(")");
}

void saveLocationData() {
  int32_t storeOffset = location.utcOffset;
  if (abs(storeOffset) < 1000) {
    Serial.print("Converting location.utcOffset from hours to seconds before saving: ");
    Serial.println(storeOffset);
    storeOffset = storeOffset * 3600;
    Serial.print("Stored timezone offset (seconds): ");
    Serial.println(storeOffset);
  }
  EEPROM.put(TIMEZONE_ADDR, storeOffset);
  EEPROM.commit();
}

void loadLocationData() {
  EEPROM.get(TIMEZONE_ADDR, location.utcOffset);
  Serial.print("Raw timezone offset read from EEPROM: ");
  Serial.println(location.utcOffset);

  if (abs(location.utcOffset) < 1000 && location.utcOffset != 0) {
    Serial.print("Interpreting EEPROM offset as hours, converting to seconds: ");
    Serial.println(location.utcOffset);
    location.utcOffset = location.utcOffset * 3600;
    Serial.print("Normalized EEPROM timezone offset (seconds): ");
    Serial.println(location.utcOffset);
  }

  TIMEZONE_SECONDS = location.utcOffset;
  if (location.utcOffset != 0) {
    debugPrint("Loaded timezone offset from EEPROM");
  }
}

// =============== DISPLAY FUNCTIONS ===============
void formatSeconds(char* buffer) {
  sprintf(buffer, "%02d", s);
}

void formatTime(char* buffer, bool showColon = true) {
  sprintf(buffer, "%02d%c%02d", h, (showColon ? ':' : ' '), m);
}

// =============== TIME SYNCHRONIZATION ===============
void syncTimeFromNTP() {
  debugPrint("Synchronizing time from NTP...");
  Serial.print("Using timezone offset (seconds): ");
  Serial.println(TIMEZONE_SECONDS);
  
  configTime(TIMEZONE_SECONDS, DST, NTP_SERVERS[0], NTP_SERVERS[1]);
  
  uint32_t startTime = millis();
  time_t now = time(nullptr);
  
  while (now < 24 * 3600 && (millis() - startTime) < 10000) {
    delay(100);
    now = time(nullptr);
  }
  
  if (now > 24 * 3600) {
    debugPrint("Time synchronized successfully");
    time_t syncTime = time(nullptr);
    struct tm* p_tm = localtime(&syncTime);
    Serial.print("Fetched UTC time: ");
    Serial.print(p_tm->tm_year + 1900);
    Serial.print("-");
    if (p_tm->tm_mon + 1 < 10) Serial.print("0");
    Serial.print(p_tm->tm_mon + 1);
    Serial.print("-");
    if (p_tm->tm_mday < 10) Serial.print("0");
    Serial.print(p_tm->tm_mday);
    Serial.print(" ");
    if (p_tm->tm_hour < 10) Serial.print("0");
    Serial.print(p_tm->tm_hour);
    Serial.print(":");
    if (p_tm->tm_min < 10) Serial.print("0");
    Serial.print(p_tm->tm_min);
    Serial.print(":");
    if (p_tm->tm_sec < 10) Serial.print("0");
    Serial.println(p_tm->tm_sec);
  } else {
    debugPrint("WARNING: Time sync timed out");
  }
}

void updateTime() {
  time_t now = time(nullptr);
  struct tm* p_tm = localtime(&now);
  
  h = p_tm->tm_hour;
  m = p_tm->tm_min;
  s = p_tm->tm_sec;
}

// =============== WIFI MANAGEMENT ===============
bool initializeWiFi() {
  debugPrint("Connecting to WiFi...");
  Serial.print("SSID: ");
  Serial.println(wifiConfig.ssid);
  Serial.print("Password: ");
  Serial.println(wifiConfig.password);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiConfig.ssid, wifiConfig.password);
  
  uint32_t startTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startTime > 10000) {
      debugPrint("WiFi connection timeout!");
      Serial.print("Final WiFi status: ");
      Serial.println(WiFi.status());
      return false;
    }
    delay(500);
    Serial.print(".");
  }
  
  Serial.println();
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  wifiConnected = true;
  return true;
}

void checkAndRestoreWiFi() {
  if (millis() - lastWiFiCheck < WIFI_RECONNECT_INTERVAL) {
    return;
  }
  
  lastWiFiCheck = millis();
  
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      debugPrint("WiFi reconnected!");
    }
  } else {
    if (wifiConnected) {
      debugPrint("WiFi disconnected! Attempting to reconnect...");
      wifiConnected = false;
    }
    WiFi.begin(wifiConfig.ssid, wifiConfig.password);
  }
}

/**
 * Citește senzorul de pe A0 și ajustează luminozitatea afișajului MAX7219
 */
void adjustBrightness() {
  static uint32_t lastLightCheck = 0;
  
  if (millis() - lastLightCheck < LIGHT_CHECK_INTERVAL) {
    return; // Nu a trecut intervalul de verificare
  }
  lastLightCheck = millis();

  // Citim valoarea de pe A0 (0 - 1023)
  int rawADC = analogRead(LIGHT_SENSOR_PIN);

  // Măsurăm și scalăm valoarea citită în intervalul 0..15 pentru afișaj
  // map(valoare, min_intrare, max_intrare, min_ieșire, max_ieșire)
  int targetBrightness = map(rawADC, ADC_MIN, ADC_MAX, BRIGHTNESS_MIN, BRIGHTNESS_MAX);

  // Limităm valoarea între BRIGHTNESS_MIN și BRIGHTNESS_MAX
  targetBrightness = constrain(targetBrightness, BRIGHTNESS_MIN, BRIGHTNESS_MAX);

  // Aplicăm noua luminozitate afișajului
  P.setIntensity(targetBrightness);

  // Debug pe Serial (opțional, util pentru calibrare)
  /*
  Serial.print("Senzor A0: ");
  Serial.print(rawADC);
  Serial.print(" -> Luminozitate setata: ");
  Serial.println(targetBrightness);
  */
}

// =============== INITIALIZATION ===============
void setup(void) {
  Serial.begin(115200);
  delay(10);
  
  debugPrint("\n\n=== ESP8266 MAX7219 NTP Clock with Auto-Geolocation ===");
  
  initEEPROM();
  loadLocationData();
  
  if (!initializeWiFi()) {
    debugPrint("ERROR: Could not connect to WiFi");
    debugPrint("Retrying with default credentials from WiFiConfig.h...");
    
    WiFi.disconnect(true);
    delay(500);
    
    strcpy(wifiConfig.ssid, DEFAULT_SSID);
    strcpy(wifiConfig.password, DEFAULT_PASSWORD);
    
    if (!initializeWiFi()) {
      debugPrint("ERROR: Could not connect with default credentials either");
      debugPrint("Using UTC as fallback timezone");
    }
  }

  if (wifiConnected) {
    if (fetchGeolocation()) {
      saveLocationData();
      Serial.print("Auto-detected timezone: UTC");
      if (location.utcOffset >= 0) Serial.print("+");
      Serial.print(location.utcOffset / 3600.0, 2);
      Serial.println("");
    } else {
      debugPrint("Geolocation failed, using fallback timezone");
    }
  }
  
  if (wifiConnected) {
    syncTimeFromNTP();
  } else {
    Serial.print("WiFi not connected. Using timezone offset: ");
    Serial.print(TIMEZONE_SECONDS);
    Serial.println(" seconds");
    configTime(TIMEZONE_SECONDS, DST, NTP_SERVERS[0], NTP_SERVERS[1]);
  }
  
  updateTime();
  Serial.print("Display time will show: ");
  if (h < 10) Serial.print("0");
  Serial.print(h);
  Serial.print(":");
  if (m < 10) Serial.print("0");
  Serial.println(m);
  
  // Initialize SPI & Parola for ESP8266
  SPI.begin();
  P.begin(4);
  // Rotire 180 grade (flip pe orizontală și verticală)
P.setZoneEffect(0, true, PA_FLIP_LR);
P.setZoneEffect(0, true, PA_FLIP_UD);

P.setZoneEffect(1, true, PA_FLIP_LR);
P.setZoneEffect(1, true, PA_FLIP_UD);
  P.setInvert(false);
  
  // Setup display zones (0=seconds, 1=time)
 // P.setZone(0, 0, 0);      // Zone 0: first LED module
  //P.setZone(1, 1, 3);      // Zone 1: remaining LED modules

  P.setZone(0, 3, 3); // Mută secunda pe modulul din dreapta/stânga (după rotire)
P.setZone(1, 0, 2); // Restul de 3 module pentru oră

  P.setFont(0, numeric7Seg);
  P.setFont(1, numeric7Se);
  
  P.displayZoneText(0, szsecond, PA_LEFT, SPEED_TIME, 0, PA_PRINT, PA_NO_EFFECT);
  P.displayZoneText(1, szTime, PA_CENTER, SPEED_TIME, PAUSE_TIME, PA_PRINT, PA_NO_EFFECT);
 // P.setIntensity(0);
 adjustBrightness();
  
  updateTime();
  formatTime(szTime);
  Serial.print("Display time (HH:MM): ");
  Serial.println(szTime);
  
  debugPrint("Setup complete!");
}

// =============== MAIN LOOP ===============
void loop(void) {
  static uint32_t lastUpdate = 0;
  static bool flasher = false;
  
  P.displayAnimate();
  checkAndRestoreWiFi();
  
adjustBrightness();

  if (millis() - lastUpdate >= 1000) {
    lastUpdate = millis();
    
    updateTime();
    formatTime(szTime, flasher);
    formatSeconds(szsecond);
    
    flasher = !flasher;
    
    P.displayZoneText(0, szsecond, PA_LEFT, SPEED_TIME, 0, PA_PRINT, PA_NO_EFFECT);
    P.displayZoneText(1, szTime, PA_CENTER, SPEED_TIME, PAUSE_TIME, PA_PRINT, PA_NO_EFFECT);
  }
}

void getTimentp() __attribute__((deprecated("Use syncTimeFromNTP() instead")));
void getTimentp() {
  syncTimeFromNTP();
}