/**************************************************************************************************
 * Improved Fingerprint Attendance System for ESP32
 * Version: 5.3 (Fully Commented)
 *
 * Description:
 * A comprehensive fingerprint attendance system using an ESP32. It is designed to be
 * robust and user-friendly, with features for online and offline operation.
 *
 * Key Features:
 * - On-Demand WiFi Setup: Uses WiFiManager to create a web portal for WiFi configuration
 * only when a dedicated button is held down, allowing for fast boot-up in normal operation.
 * - Offline Logging: If no WiFi is available, attendance logs are saved to an SD card.
 * - Automatic Sync: Saved offline logs are automatically sent to the server once an
 * internet connection is established.
 * - Accurate Timestamps: Uses a DS3231 Real-Time Clock (RTC) with a backup battery
 * to ensure timestamps are always accurate, even after power loss.
 * - Secure Server Communication: Uses HTTPS for secure data transmission.
 * - User-Friendly Interface: A 16x2 LCD provides clear instructions and feedback.
 * - Low-Power Mode: Enters light sleep when idle to conserve battery, waking on button press.
 *
 **************************************************************************************************/

// --- LIBRARY INCLUSIONS ---
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal.h>
#include <Adafruit_Fingerprint.h>
#include <RTClib.h>
#include <SD.h>
#include <SPI.h>
#include <time.h> 

// --- HARDWARE PIN DEFINITIONS ---
// LCD Pins (rs, en, d4, d5, d6, d7)
const uint8_t rs = 27, en = 26, d4 = 25, d5 = 33, d6 = 32, d7 = 14;
// Fingerprint Sensor RX/TX (connected to ESP32's Serial Port 2)
const uint8_t FINGERPRINT_RX = 16;
const uint8_t FINGERPRINT_TX = 17;
// Button 1: Main operational button (for enrolling, menus, etc.)
const uint8_t BUTTON_PIN1 = 34; // GPIO34 is input-only, requires external pull-up resistor
// Button 2: WiFiManager setup portal (long press)
const uint8_t BUTTON_PIN2 = 35; // GPIO35 is input-only, requires external pull-up resistor
// SD Card Chip Select (CS) Pin
const uint8_t SD_CS_PIN = 5;
// Define the GPIO Pin that will be connected to the Interrupt Pin of the fingerprint sensor
const uint8_t FINGERPRINT_IRQ_PIN = 0;
// Pin to control LCD backlight
const uint8_t LCD_BACKLIGHT_PIN = 13;

// --- SERVER AND TIME CONFIGURATION ---
#define SERVER_HOST "https://192.168.1.12:7069" // The base URL of your backend server
#define NTP_SERVER "pool.ntp.org"             // Network Time Protocol server for initial time sync
#define GMT_OFFSET_SEC (3600 * 2)               // GMT offset for your timezone (e.g., UTC+2 for Egypt Standard Time)
#define DAYLIGHT_OFFSET_SEC (3600 * 1)                // Daylight saving offset (0 if not applicable)
#define OFFLINE_LOG_FILE "/attendance_log.txt"

// --- CONSTANTS AND TIMERS ---
const uint32_t DEBOUNCE_DELAY = 50;        // 50 ms for button debouncing
const uint32_t LONG_PRESS_DELAY = 1000;    // 1000 ms = 1 second for a long press
const uint32_t MENU_TIMEOUT = 10000;       // 10000 ms = 10 seconds for menu timeout
const uint32_t SLEEP_TIMEOUT = 15000;       // 15000 ms = 15 seconds of inactivity before light sleep
const long gmtOffset_sec = 2 * 3600;        // GMT+2 (توقيت شرق أوروبا القياسي)
const int daylightOffset_sec = 1 * 3600;    //ساعة واحدة للتوقيت الصيفي (EEST)
static unsigned long lastSignalUpdateTime = 0;

// WiFi Signal Symbol - No Signal (أو مجرد نقطة)
byte wifiStrong[8] = {
  0b00000,
  0b00000,
  0b00000, // السطر ده عشان فيه 8 صفوف للـ char
  0b01110, // الموجة العليا
  0b10001, // بداية الموجة الوسطى
  0b01010, // الموجة الوسطى (نقطتين)
  0b00100, // الموجة السفلية (نقطة واحدة)
  0b00000
};

byte wifiMedium[8] = {
  0b00000,
  0b00000,
  0b00000,
  0b00000, // الموجة العليا فاضية
  0b01010, // الموجة الوسطى (نقطتين)
  0b00100, // الموجة السفلية (نقطة واحدة)
  0b00000,
  0b00000
};

byte wifiWeak[8] = {
  0b00000,
  0b00000,
  0b00000,
  0b00000,
  0b00000, // الموجة العليا والوسطى فاضية
  0b00100, // نقطة واحدة فقط
  0b00000,
  0b00000
};
// تعريف رمز البصمة الجديد (شكل أكثر جاذبية)
byte fingerprintSymbol[8] = {
  0b00100, // . . X . . (نقطة مركزية أو بداية خط)
  0b01010, // . X . X . (قوس علوي)
  0b10001, // X . . . X (قوس أوسع)
  0b10101, // X . X . X (نقطة في المنتصف مع خطوط جانبية)
  0b10001, // X . . . X
  0b01010, // . X . X .
  0b00100, // . . X . .
  0b00000  // صف فارغ
};

// --- GLOBAL OBJECTS ---
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);
HardwareSerial fingerSerial(2); // Use Serial Port 2 for the fingerprint sensor
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);
RTC_DS3231 rtc;

// --- STATE MANAGEMENT ---
enum class MenuState {
  MAIN_MENU,
  OPTIONS_MENU
};

MenuState currentMenuState = MenuState::MAIN_MENU;
uint32_t lastActivityTime = 0; // Tracks time for sleep and menu timeouts

// --- BUTTON STATE VARIABLES ---
uint8_t btn1State = HIGH;
uint8_t btn2State = HIGH;
uint32_t btn1PressTime = 0;
uint32_t btn2PressTime = 0;
bool btn1Held = false;
bool btn2Held = false;

// --- FUNCTION PROTOTYPES ---
// Core System & UI
void displayMessage(String line1, String line2 = "", int delayMs = 0);
void handleButtons();
uint16_t findNextAvailableID();
void displayWiFiSignal();

// WiFi & Server
void setupWiFi(bool portal);
bool logToServer(const String& endpoint, const String& payload);
bool syncAttendanceToServer(uint16_t id, String timestampString);
uint16_t fetchLastIdFromServer();

// Fingerprint Operations
void scanForFingerprint();
void enrollNewFingerprint();
uint8_t getFingerprintImage(int step);
uint8_t createAndStoreModel(uint16_t id);

// SD Card and Offline Logging
void syncOfflineLogs();
void attemptToClearAllData();
void setupModules();
void setupRtcAndSyncTime();
void logAttendanceOffline(uint16_t id, time_t timestamp);

// Menu Actions
void showMainMenu();
void showOptionsMenu();

// Power Management
void enterLightSleep();

/**************************************************************************************************
 * SETUP: Runs once on boot. Initializes all hardware and software components.
 **************************************************************************************************/
void setup() {
 Serial.begin(115200); // For debugging purposes
  digitalWrite(LCD_BACKLIGHT_PIN, LOW);
  setupModules();
  setupRtcAndSyncTime();
  
  // Initial WiFi connection attempt (non-blocking)
  displayMessage("Connecting WiFi", "Please wait...");
  //setupWiFi(false); // false = don't start config portal on boot

  WiFiManager wm;
  wm.setConnectTimeout(20); 
  wm.setConfigPortalBlocking(false); 
  bool connected = wm.autoConnect("FingerprintSetupAP");
  if (connected) {
    displayMessage("WiFi Connected!", WiFi.localIP().toString(), 2000);
    setenv("TZ", "EET-2EEST,M4.4.5/0,M10.5.5/1", 1);
    tzset(); 
    configTime(gmtOffset_sec, daylightOffset_sec, NTP_SERVER);
    syncOfflineLogs(); // Sync any logs stored on SD card
  } else {
    displayMessage("Offline Mode", "WI FI not Active", 2000);
  }

  lastActivityTime = millis(); // Initialize activity timer
  showMainMenu();
}

/**************************************************************************************************
 * LOOP: Main program cycle. Handles buttons, fingerprint scanning, and power management.
 **************************************************************************************************/
void loop() {
  handleButtons();
  lcd.setCursor(15, 1); 
  lcd.write(4);
  if (WiFi.status() != WL_CONNECTED) {
       WiFi.reconnect();
     }
  if (WiFi.status() == WL_CONNECTED && millis() - lastSignalUpdateTime > 1000) { 
    displayWiFiSignal();
    lastSignalUpdateTime = millis();
  }
  // Only scan for fingerprints when in the main menu and no buttons are being pressed.
  if (currentMenuState == MenuState::MAIN_MENU && !btn1PressTime && !btn2PressTime) {
    scanForFingerprint();
  }

  if (currentMenuState == MenuState::MAIN_MENU && WiFi.status() == WL_CONNECTED) {
     displayWiFiSignal(); 
  }
  // Check for menu timeout in the options menu
  if (currentMenuState == MenuState::OPTIONS_MENU && (millis() - lastActivityTime > MENU_TIMEOUT)) {
      displayMessage("Timeout", "Returning...", 1500);
      showMainMenu();
  }
  
  // Check for inactivity to enter light sleep
  if (millis() - lastActivityTime > SLEEP_TIMEOUT) {
    enterLightSleep();
  }
}

/**************************************************************************************************
 * Core System & UI Functions                                    *
 **************************************************************************************************/

/**
 * @brief Initializes LCD, SD Card, Fingerprint Sensor, and Buttons.
 */
void setupModules() {
  // --- LCD Setup ---
  lcd.begin(16, 2);
   lcd.createChar(0, wifiWeak);   
  lcd.createChar(1, wifiMedium); 
  lcd.createChar(2, wifiStrong);
  lcd.createChar(4, fingerprintSymbol);
  digitalWrite(LCD_BACKLIGHT_PIN, LOW);
  displayMessage("System Booting", "Please wait...");
  delay(2000);

  // --- SD Card Setup ---
  if (!SD.begin(SD_CS_PIN)) {
    displayMessage("SD Card Error!", "Check wiring.", 5000);
  }

  // --- Fingerprint Sensor Setup ---
  fingerSerial.begin(57600, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
  if (finger.verifyPassword()) {
    displayMessage("Fingerprint", "Sensor OK!", 1500);
  } else {
    displayMessage("Fingerprint", "Sensor Error!", 5000);
    while(1) { delay(1); } // Halt on critical error
  }
  finger.getTemplateCount(); // Get number of stored templates
  // Serial.print("Sensor contains "); Serial.print(finger.templateCount); Serial.println(" templates.");


  // --- Button Setup ---
  // GPIO 34 & 35 are input only and need external pull-up resistors.
  pinMode(BUTTON_PIN1, INPUT_PULLUP); 
  pinMode(BUTTON_PIN2, INPUT_PULLUP);
  pinMode(FINGERPRINT_IRQ_PIN, INPUT_PULLUP);
  pinMode(LCD_BACKLIGHT_PIN, OUTPUT);

}

/**
 * @brief Initializes the RTC and syncs it with an NTP server if online.
 */
void setupRtcAndSyncTime() {
  if (!rtc.begin()) {
    displayMessage("RTC Error!", "Check wiring.", 5000);
    //while (1) { delay(1); } // Halt on critical error
  }
  
  if (rtc.lostPower()) {
    displayMessage("RTC Power Lost!", "Syncing time...", 2000);
    if (WiFi.status() == WL_CONNECTED) {
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)){
          rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
          displayMessage("RTC Synced!", "From NTP.", 1500);
          return; 
      } else {
          displayMessage("NTP Sync Failed!", "Using compile time.", 2000);
      }
    } else {
      displayMessage("No WiFi for NTP", "Using compile time.", 2000);
    }
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    displayMessage("RTC Set", "From Compile Time", 1500);

  } else {

  }
}

/**
 * @brief Displays a two-line message on the LCD. Clears the screen first.
 * @param line1 Text for the first row.
 * @param line2 Text for the second row (optional).
 * @param delayMs Duration to show the message before returning (optional).
 */
void displayMessage(String line1, String line2, int delayMs) {
  digitalWrite(LCD_BACKLIGHT_PIN, LOW);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  if (delayMs > 0) {
    delay(delayMs);
  }
  if (WiFi.status() == WL_CONNECTED) {
      displayWiFiSignal(); 
  }
}

/**
 * @brief Enters light sleep to save power and configures buttons to wake the ESP32.
 */
// const int BUTTON_PIN1 = 34;
// const int BUTTON_PIN2 = 35;

void enterLightSleep() {
    
    DateTime now = rtc.now();
    char timeStr[12]; 
    int hour12 = now.hour();
    String ampm = "AM";
    if (hour12 >= 12) {
        ampm = "PM";
        if (hour12 > 12) {
            hour12 -= 12;
        }
    }
    if (hour12 == 0) { // Handle 00:XX AM as 12:XX AM
        hour12 = 12;
    }
    sprintf(timeStr, "%02d:%02d:%02d %s", hour12, now.minute(), now.second(), ampm.c_str());

    // تجهيز التاريخ
    char dateStr[11]; // Buffer for YYYY-MM-DD
    sprintf(dateStr, "%04d-%02d-%02d", now.year(), now.month(), now.day());
    int timeLen = String(timeStr).length();
    int dateLen = String(dateStr).length();
    int timePadding = (16 - timeLen) / 2;
    int datePadding = (16 - dateLen) / 2;

    lcd.clear();
    lcd.setCursor(timePadding, 0); 
    lcd.print(timeStr);
    lcd.setCursor(datePadding, 1); 
    lcd.print(dateStr);

    lcd.setCursor(15, 1);
    lcd.write(4);
    if (WiFi.status() == WL_CONNECTED) {
        displayWiFiSignal(); 
    }
    delay(2000); 
    /*esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_34, 0); 
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_35, 0);
    esp_light_sleep_start();*/
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL); 
    uint64_t wakeup_pins_mask = (1ULL << FINGERPRINT_IRQ_PIN);
    digitalWrite(LCD_BACKLIGHT_PIN, HIGH);
    esp_sleep_enable_ext1_wakeup(wakeup_pins_mask, ESP_EXT1_WAKEUP_ALL_LOW);
    esp_light_sleep_start();
    digitalWrite(LCD_BACKLIGHT_PIN, LOW);
    esp_sleep_source_t wakeup_reason = esp_sleep_get_wakeup_cause();

    if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
        uint64_t wakeup_gpio_mask = esp_sleep_get_ext1_wakeup_status();

        if (wakeup_gpio_mask & (1ULL << FINGERPRINT_IRQ_PIN)) {
            displayMessage("Finger Detected!", "Scanning...", 1000);
            scanForFingerprint(); 

        } else if (wakeup_gpio_mask & ((1ULL << GPIO_NUM_34) | (1ULL << GPIO_NUM_35))) {
            displayMessage("Button Pressed!", "Waking Up...", 1000);
            showMainMenu(); 
        }
    } else {

        displayMessage("Woke Up!", "Returning...", 1000);
        showMainMenu();
    }

     if (WiFi.status() != WL_CONNECTED) {
       displayMessage("Reconnecting WiFi", "", 0);
       WiFi.reconnect();
     }

    lcd.display();
    lastActivityTime = millis(); 
    showMainMenu(); 
}


/**************************************************************************************************
 * Button Handling Logic                                        *
 **************************************************************************************************/

/**
 * @brief Manages button states, debouncing, and press/hold detection.
 */
void handleButtons() {
    // --- Read current button states ---
    uint8_t currentBtn1 = digitalRead(BUTTON_PIN1);
    uint8_t currentBtn2 = digitalRead(BUTTON_PIN2);

    // --- Button 1 Logic (Add/Options) ---
    if (currentBtn1 != btn1State) {
        btn1PressTime = millis(); // Start timer on state change
        btn1Held = false;
    }
    btn1State = currentBtn1;

    if (btn1PressTime && (millis() - btn1PressTime > DEBOUNCE_DELAY)) {
        if (btn1State == LOW) { // Button is pressed
            if (!btn1Held && (millis() - btn1PressTime > LONG_PRESS_DELAY)) {
                // --- LONG PRESS ACTION ---
                btn1Held = true;
                lastActivityTime = millis();
                if (currentMenuState == MenuState::MAIN_MENU) {
                    showOptionsMenu();
                } else if (currentMenuState == MenuState::OPTIONS_MENU) {
                    attemptToClearAllData();
                }
            }
        } else { // Button is released
            if (!btn1Held) {
                // --- SHORT PRESS ACTION ---
                lastActivityTime = millis();
                if (currentMenuState == MenuState::MAIN_MENU) {
                    enrollNewFingerprint();
                }
            }
            btn1PressTime = 0; // Reset timer
        }
    }

    // --- Button 2 Logic (WiFi Manager) ---
    if (currentBtn2 != btn2State) {
        btn2PressTime = millis();
        btn2Held = false;
    }
    btn2State = currentBtn2;

    if (btn2PressTime && (millis() - btn2PressTime > DEBOUNCE_DELAY)) {
         if (btn2State == LOW) { // Button is pressed
            if (!btn2Held && (millis() - btn2PressTime > LONG_PRESS_DELAY)) {
                 // --- LONG PRESS ACTION ---
                btn2Held = true;
                lastActivityTime = millis();
                setupWiFi(true); // true = start config portal
                showMainMenu();
            }
        } else { // Button is released
             btn2PressTime = 0; // Reset timer
        }
    }
}


/**************************************************************************************************
 * WiFi & Server Functions                                      *
 **************************************************************************************************/

/**
 * @brief Sets up WiFi. Can start a configuration portal if requested.
 * @param portal If true, starts the WiFiManager portal for configuration.
 */
void setupWiFi(bool portal) {
  WiFiManager wm;
  wm.setConnectTimeout(20); // Set connect timeout to 20 seconds
  
  if (portal) {
    displayMessage("Setup Mode", "Connect to AP...", 0);
    // Starts a blocking portal. ESP will reset after successful configuration.
    if (!wm.startConfigPortal("FingerprintSetupAP")) {
      displayMessage("Setup Failed", "No credentials.", 2000);
    } else {
      displayMessage("Setup Success!", "Rebooting...", 2000);
      delay(1000);
      ESP.restart();
    }
  } else {
    // Auto-connects with saved credentials without blocking
    wm.autoConnect("FingerprintSetupAP");
  }

  // After any connection attempt, sync time if connected
  if (WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)){
        rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
        displayMessage("Time Synced!", "", 1500);
    }
  }
}

/**
 * @brief Fetches the last used fingerprint ID from the server.
 * @return The last ID as a uint16_t, or 0 on failure.
 */
uint16_t fetchLastIdFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    displayMessage("No WiFi", "Cannot get ID", 2000);
    return 0;
  }
  
  HTTPClient http;
  String url = String(SERVER_HOST) + "/api/SensorData/last-id";
  
  http.begin(url); 

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    http.end();
    return (uint16_t)payload.toInt();
  } else {
    displayMessage("Server Error", "Code: " + String(httpCode), 2000);
    http.end();
    return 0;
  }
}

/**
 * @brief Sends attendance data to the server.
 * @param id The user's fingerprint ID.
 * @param timestamp The Unix timestamp of the attendance.
 * @return True on success, false on failure.
 */
bool syncAttendanceToServer(uint16_t id, String timestampString) {
    if (WiFi.status() != WL_CONNECTED) return false;

    StaticJsonDocument<128> doc;
    doc["FingerID"] = id;               
    doc["Timestamp"] = timestampString; 

    String payload;
    serializeJson(doc, payload);

    return logToServer("/api/SensorData", payload);
}

/**
 * @brief Sends a generic POST request to a server endpoint.
 * @param endpoint The API endpoint (e.g., "/api/SensorData/enroll").
 * @param payload The JSON string to send in the request body.
 * @return True if the server returns a 2xx success code, false otherwise.
 */
bool logToServer(const String& endpoint, const String& payload) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected. This should not happen if device reports connected.");
        return false;
    }
    
    HTTPClient http;
    String url = String(SERVER_HOST) + endpoint;
    
    Serial.print("DEBUG: Full URL being attempted: ");
    Serial.println(url);
    Serial.print("DEBUG: Payload being sent: ");
    Serial.println(payload);
    bool beginSuccess;
    http.begin(url); 
    http.addHeader("Content-Type", "application/json");

    int httpCode = http.POST(payload);
    
    Serial.print("DEBUG: HTTP Response Code: ");
    Serial.println(httpCode); 
    
    if (httpCode > 0) { 
        String response = http.getString(); 
        Serial.print("DEBUG: Server Response: ");
        Serial.println(response);
    } else {
        Serial.print("DEBUG: HTTP Connection Error: ");
        Serial.println(http.errorToString(httpCode).c_str());
    }

    http.end();

    return (httpCode >= 200 && httpCode < 300);
}

void displayWiFiSignal() {
  if (WiFi.status() == WL_CONNECTED) {
    long rssi = WiFi.RSSI(); 
    uint8_t signalCharIndex = 0; 

    if (rssi >= -60) {
      signalCharIndex = 2; 
    } else if (rssi >= -75) { 
      signalCharIndex = 1; 
    } else {
      signalCharIndex = 0; 
    }

    lcd.setCursor(15, 0); 
    lcd.write(signalCharIndex); 
  } else {
    
    lcd.setCursor(15, 0);
    lcd.print(" "); 
  }
}

/**************************************************************************************************
 * Fingerprint Operation Functions                                 *
 **************************************************************************************************/


/**
 * @brief Finds the next available (empty) ID slot on the fingerprint sensor.
 * @return The first available ID (0-127), or 128 if all slots are full.
 */

/**
 * @brief Continuously scans for a fingerprint and logs attendance if a match is found.
 */
void scanForFingerprint() {
  lcd.setCursor(15, 1); 
  lcd.write(4);
  // Wait for a finger to be placed on the sensor
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return;

  // Convert the image to a feature template
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return;

  // Search the sensor's internal database for a match
  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) {
    lastActivityTime = millis();
    uint16_t fingerID = finger.fingerID;
    displayMessage("Welcome!", "ID: " + String(fingerID), 2000);
    
    // Log the attendance (this function handles online/offline logic)
    DateTime now = rtc.now();
    String timestampString = String(now.year()) + "-" +
                             (now.month() < 10 ? "0" : "") + String(now.month()) + "-" +
                             (now.day() < 10 ? "0" : "") + String(now.day()) + "T" +
                             (now.hour() < 10 ? "0" : "") + String(now.hour()) + ":" +
                             (now.minute() < 10 ? "0" : "") + String(now.minute()) + ":" +
                             (now.second() < 10 ? "0" : "") + String(now.second()) + ".000Z";
    if(syncAttendanceToServer(fingerID, timestampString)){
        displayMessage("Attendance Sent!", "", 1500);
    } else {
        displayMessage("Saved Offline", "", 1500);
        logAttendanceOffline(fingerID, now.unixtime());
    }
    showMainMenu();
  } else if (p == FINGERPRINT_NOTFOUND) { 
    displayMessage("Fingerprint", "Not Found!", 1500); 
    delay(500); //Small delay to prevent rapid failed reads if a finger is held down
    showMainMenu();
  }
  else {
    // Other errors or states
    displayMessage("Scan Error", "Try Again!", 1500); 
    showMainMenu();
  }
}


/**
 * @brief Manages the multi-step process of enrolling a new user.
 */
void enrollNewFingerprint() {
    lcd.setCursor(15, 1); 
    lcd.write(4);
    displayMessage("Enrollment:", "Checking server...", 0); 

    uint16_t newId = 0; 

    if (WiFi.status() != WL_CONNECTED) {
        displayMessage("No WiFi!", "Cannot enroll.", 2000);
        showMainMenu();
        return; 
    }

    displayMessage("Enrollment:", "Fetching ID...", 0); 
    newId = fetchLastIdFromServer() + 1;
    
    lastActivityTime = millis(); 

    displayMessage("Place finger", "ID: " + String(newId));
    if (getFingerprintImage(1) != FINGERPRINT_OK) {
        displayMessage("Enroll Failed", "Try Again.", 2000); 
        showMainMenu();
        return;
    }

    displayMessage("Place again", "ID: " + String(newId));
    if (getFingerprintImage(2) != FINGERPRINT_OK) {
        displayMessage("Enroll Failed", "Try Again.", 2000); 
        showMainMenu();
        return;
    }

    displayMessage("Creating model", "Please wait...");
    uint8_t createStoreResult = createAndStoreModel(newId);

      if (createStoreResult == FINGERPRINT_OK) {
        
          displayMessage("Local Store OK!", "Syncing...", 1000);

          
          StaticJsonDocument<64> doc;
          doc["FingerID"] = newId; 

          String payload;
          serializeJson(doc, payload);
              
          displayMessage("Syncing to Server", "Please wait...", 0);

          if (logToServer("/api/SensorData", payload)) { 
              displayMessage("Server Synced!", "Enrolled!", 2000);
          } else {
              
              displayMessage("Server Sync Failed", "Local OK, Server NO", 3000);
          }
      } else {
          
      }
    showMainMenu();
}
/**
 * @brief Gets a single fingerprint image and converts it to a template.
 * @param step The step in the enrollment process (1 or 2).
 * @return Fingerprint status code (e.g., FINGERPRINT_OK).
 */
uint8_t getFingerprintImage(int step) {
  uint8_t p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    // Allow user to cancel by pressing button 1
    if (digitalRead(BUTTON_PIN1) == LOW) return FINGERPRINT_ENROLLMISMATCH;
  }
  
  p = finger.image2Tz(step);
  if (p == FINGERPRINT_OK) {
    displayMessage("Image taken", "", 1000);
    return FINGERPRINT_OK;
  } else {
    displayMessage("Error", "Could not process", 2000);
    return p;
  }
}

/**
 * @brief Combines the two stored templates into a single model and saves it to flash.
 * @param id The ID to store the model against.
 * @return Fingerprint status code.
 */
uint8_t createAndStoreModel(uint16_t id) {
  uint8_t p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    displayMessage("Model Error", "Try again", 2000);
    return p;
  }

  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    return p;
  } else {
    displayMessage("Storage Error", "Code: " + String(p), 2000);
    return p;
  }
}


/**************************************************************************************************
 * SD Card and Offline Logging                                      *
 **************************************************************************************************/

/**
 * @brief Logs an attendance record to the SD card.
 * @param id The user's fingerprint ID.
 * @param timestamp The Unix timestamp of the attendance.
 */
void logAttendanceOffline(uint16_t id, time_t timestamp) {
  File logFile = SD.open(OFFLINE_LOG_FILE, FILE_APPEND);
  if (logFile) {
    logFile.print(String(id) + "," + String(timestamp) + "\n");
    logFile.close();
  } else {
    displayMessage("SD Write Error!", "", 2000);
  }
}

/**
 * @brief Reads logs from the SD card and attempts to sync them to the server.
 */
void syncOfflineLogs() {
  lcd.setCursor(15, 1); 
  lcd.write(4);
  displayMessage("Syncing Offline ", "logs...", 0);
  File logFile = SD.open(OFFLINE_LOG_FILE, FILE_READ);
  if (!logFile) {
    displayMessage("No offline logs", "to sync.", 1500);
    return;
  }

  String tempLogName = "/temp_log.txt";
  File tempFile = SD.open(tempLogName, FILE_WRITE);
  bool allSynced = true;

  while (logFile.available()) {
    String line = logFile.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int commaIndex = line.indexOf(',');
    if (commaIndex != -1) {
      uint16_t id = line.substring(0, commaIndex).toInt();
      time_t timestamp = (time_t)line.substring(commaIndex + 1).toInt();
      DateTime entryTime(timestamp); 
      String timestampString = String(entryTime.year()) + "-" +
                               (entryTime.month() < 10 ? "0" : "") + String(entryTime.month()) + "-" +
                               (entryTime.day() < 10 ? "0" : "") + String(entryTime.day()) + "T" +
                               (entryTime.hour() < 10 ? "0" : "") + String(entryTime.hour()) + ":" +
                               (entryTime.minute() < 10 ? "0" : "") + String(entryTime.minute()) + ":" +
                               (entryTime.second() < 10 ? "0" : "") + String(entryTime.second()) + ".000Z";
      if (syncAttendanceToServer(id, timestampString)) {
        // Log was sent successfully, so we don't write it to the temp file.
      } else {
        // Failed to send, so write it to the temp file to keep it.
        allSynced = false;
        if(tempFile) tempFile.println(line);
      }
    }
  }

  logFile.close();
  if(tempFile) tempFile.close();

  // Replace the old log file with the new one containing only unsynced logs.
  SD.remove(OFFLINE_LOG_FILE);
  if (!allSynced) {
    SD.rename(tempLogName, OFFLINE_LOG_FILE);
    displayMessage("Sync complete.", "Some logs remain.", 2000);
  } else {
    SD.remove(tempLogName); // Remove the empty temp file
    displayMessage("Sync complete!", "All logs sent.", 2000);
  }
}

/**************************************************************************************************
 * Menu Actions                                              *
 **************************************************************************************************/

/**
 * @brief Shows the options menu on the LCD.
 */
void showOptionsMenu() {
  currentMenuState = MenuState::OPTIONS_MENU;
  digitalWrite(LCD_BACKLIGHT_PIN, LOW);
  lcd.setCursor(15, 1); 
  lcd.write(4);
  displayMessage("Hold btn1: Clear", "Timeout: 10s");
  lastActivityTime = millis(); // Reset timer for menu timeout
   if (WiFi.status() == WL_CONNECTED) {
      displayWiFiSignal();
  }
}
/**
 * @brief Shows the main menu on the LCD.
 */
void showMainMenu() {
  currentMenuState = MenuState::MAIN_MENU;
  digitalWrite(LCD_BACKLIGHT_PIN, LOW);
  lcd.setCursor(15, 1); 
  lcd.write(4);
  displayMessage("btn1:add finger", "btn2:manage wifi ");
  lastActivityTime = millis();
   if (WiFi.status() == WL_CONNECTED) {
      displayWiFiSignal();
  }
}
/**
 * @brief Contacts the server for authorization and then deletes all fingerprint data.
 */
void attemptToClearAllData() {
  displayMessage("Authorizing...", "Please wait", 0);
    lcd.setCursor(15, 1); 
    lcd.write(4);
  if (WiFi.status() != WL_CONNECTED) {
      displayMessage("No WiFi", "Cannot clear data", 2000);
      showMainMenu();
      return;
  }

  HTTPClient http;
  String url = String(SERVER_HOST) + "/api/SensorData/clear";
  http.begin(url);
  int httpCode = http.POST("");
  http.end();
  
  if (httpCode >= 200 && httpCode < 300) {
      displayMessage("Authorized!", "Deleting data...", 1000);
      if (finger.emptyDatabase() == FINGERPRINT_OK) {
          displayMessage("All data cleared!", "", 2000);
      } else {
          displayMessage("Delete Failed", "Sensor error", 2000);
      }
  } else {
      displayMessage("Authorization", "Failed! Code: " + String(httpCode), 3000);
  }
  
  showMainMenu();
}