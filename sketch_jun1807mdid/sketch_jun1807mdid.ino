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
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <SD.h>
#include <SPI.h>

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

// --- SERVER AND TIME CONFIGURATION ---
#define SERVER_HOST "https://192.168.1.12:7069" // The base URL of your backend server
#define NTP_SERVER "pool.ntp.org"             // Network Time Protocol server for initial time sync
#define GMT_OFFSET_SEC 3600 * 2               // GMT offset for your timezone (e.g., UTC+2 for Egypt Standard Time)
#define DAYLIGHT_OFFSET_SEC 0                 // Daylight saving offset (0 if not applicable)
#define OFFLINE_LOG_FILE "/attendance_log.txt"

// --- CONSTANTS AND TIMERS ---
const uint32_t DEBOUNCE_DELAY = 50;        // 50 ms for button debouncing
const uint32_t LONG_PRESS_DELAY = 1000;    // 1000 ms = 1 second for a long press
const uint32_t MENU_TIMEOUT = 10000;       // 10000 ms = 10 seconds for menu timeout
const uint32_t FULL_SLEEP_TIMEOUT = 300000UL; ;       // 15000 ms = 15 seconds of inactivity before light sleep fully sleep
const uint32_t DISPLAY_ON_IDLE_TIMEOUT = 30000UL;  // 15 seconds: The length of time the device displays the time and animation after the activity has ended
const uint32_t ANIMATION_FRAME_DURATION = 200;  // 200ms The duration of each frame in the animation in milliseconds
const int TOTAL_ANIMATION_FRAMES = 4;           // any number of frames you want for your animation.
unsigned long lastAnimationUpdateTime = 0;   // Variable to track the last time the animation frame was updated.
int currentAnimationFrame = 0;               // currentAnimationFrame

// --- Custom Characters Definitions ---
// الابتسامة
byte smiley[8] = {
  0b00000,
  0b00000,
  0b01010,
  0b00000,
  0b00000,
  0b10001,
  0b01110,
  0b00000
};

// العبوس
byte frownie[8] = {
  0b00000,
  0b00000,
  0b01010,
  0b00000,
  0b00000,
  0b00000,
  0b01110,
  0b10001
};

// الأذرع لأسفل
byte armsDown[8] = {
  0b00100,
  0b01010,
  0b00100,
  0b00100,
  0b01110,
  0b10101,
  0b00100,
  0b01010
};

// الأذرع لأعلى
byte armsUp[8] = {
  0b00100,
  0b01010,
  0b00100,
  0b10101,
  0b01110,
  0b00100,
  0b00100,
  0b01010
};

// --- GLOBAL OBJECTS ---
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);
HardwareSerial fingerSerial(2); // Use Serial Port 2 for the fingerprint sensor
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);
RTC_DS3231 rtc;
WiFiUDP ntpUDP;
NTPClient client(ntpUDP, NTP_SERVER, GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC);
// --- STATE MANAGEMENT ---
enum class MenuState {
  MAIN_MENU,
  OPTIONS_MENU,
  DISPLAY_IDLE_MODE
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
void showCurrentTimeAndAnimation(); 
void updateActivityTime();

// WiFi & Server
void setupWiFi(bool portal);
bool logToServer(const String& endpoint, const String& payload);
bool syncAttendanceToServer(uint16_t id, time_t timestamp);
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
  // Serial.begin(115200); // For debugging purposes
  
  setupModules();
  setupRtcAndSyncTime();

  lcd.begin(16, 2);
  lcd.print("Initializing...");
  
  // *** إضافة إنشاء الأشكال المخصصة هنا ***
  lcd.createChar(0, smiley);    // الابتسامة هتكون رقم 0
  lcd.createChar(1, frownie);   // العبوس هيكون رقم 1
  lcd.createChar(2, armsDown);  // الأذرع لأسفل هتكون رقم 2
  lcd.createChar(3, armsUp);    // الأذرع لأعلى هتكون رقم 3
  
  // Initial WiFi connection attempt (non-blocking)
  displayMessage("Connecting WiFi", "Please wait...");
  setupWiFi(false); // false = don't start config portal on boot
  
  if (WiFi.status() == WL_CONNECTED) {
    displayMessage("WiFi Connected!", WiFi.localIP().toString(), 2000);
    client.begin(); 
    client.update();
    syncOfflineLogs(); // Sync any logs stored on SD card
  } else {
    displayMessage("Offline Mode", "RTC Time Active", 2000);
  }

  lastActivityTime = millis(); // Initialize activity timer
  showMainMenu();
}


/**************************************************************************************************
 * LOOP: Main program cycle. Handles buttons, fingerprint scanning, and power management.
 **************************************************************************************************/


void loop() {
  handleButtons(); // لمعالجة ضغطات الأزرار وتحديث lastActivityTime
   if (WiFi.status() == WL_CONNECTED) { // حدث الوقت فقط لو فيه اتصال واي فاي
    client.update(); 
  }
  if (currentMenuState == MenuState::MAIN_MENU && (millis() - lastActivityTime <= DISPLAY_ON_IDLE_TIMEOUT)) {
      scanForFingerprint(); 
  }

  else if (millis() - lastActivityTime > DISPLAY_ON_IDLE_TIMEOUT &&
           millis() - lastActivityTime <= FULL_SLEEP_TIMEOUT) {
    
    if (currentMenuState != MenuState::DISPLAY_IDLE_MODE) {
        currentMenuState = MenuState::DISPLAY_IDLE_MODE; 
    }
    showCurrentTimeAndAnimation(); 

    // في هذه الحالة، يمكنك توفير بعض الطاقة عن طريق إيقاف تشغيل الواي فاي إذا لم تكن بحاجة إليه
    // if (WiFi.status() == WL_CONNECTED) {
    //   WiFi.disconnect(true);
    //   WiFi.mode(WIFI_OFF);
    // }
  }

  // الحالة الثالثة: وضع النوم الخفيف الكامل (Full Light Sleep)
  // إذا تجاوز millis() - lastActivityTime قيمة FULL_SLEEP_TIMEOUT، فهذا يعني خمولاً طويلاً.
  else if (millis() - lastActivityTime > FULL_SLEEP_TIMEOUT) {
    // يمكنك عرض رسالة سريعة هنا قبل الدخول في النوم
    displayMessage("Entering full sleep", "Bye bye!", 2000);
    lcd.noDisplay(); // إطفاء الشاشة بالكامل لتوفير الطاقة القصوى قبل النوم

    enterLightSleep(); // <--- هنا يتم استدعاء الدالة لإدخال الجهاز في وضع النوم الخفيف الفعلي
    // عند الاستيقاظ من enterLightSleep()، يستكمل الكود التنفيذ من بعد هذا السطر.
    // ويتم تحديث lastActivityTime و reset للحالة بعد الاستيقاظ:
    updateActivityTime(); // إعادة ضبط مؤقتات النشاط بعد الاستيقاظ
    showMainMenu();       // للعودة إلى القائمة الرئيسية
  }

  // التحقق من انتهاء مهلة القائمة في قائمة الخيارات (لا يزال هذا الجزء موجوداً في loop)
  // يجب أن يكون هذا الشرط منفصلاً ليعمل في أي حالة.
  if (currentMenuState == MenuState::OPTIONS_MENU && (millis() - lastActivityTime > MENU_TIMEOUT)) {
      displayMessage("Timeout", "Returning...", 1500);
      showMainMenu();
      updateActivityTime(); // حدث وقت النشاط هنا أيضاً لكي لا يدخل في وضع الخمول مباشرة بعد العودة
  }
}


/**************************************************************************************************
 *                                  Core System & UI Functions                                    *
 **************************************************************************************************/


/**
 * @brief Initializes LCD, SD Card, Fingerprint Sensor, and Buttons.
 */
void setupModules() {
  // --- LCD Setup ---
  lcd.begin(16, 2);
  displayMessage("System Booting", "Please wait...");
  delay(2000);

  // --- SD Card Setup ---
  if (!SD.begin(SD_CS_PIN)) {
    displayMessage("SD Card Error!", "Check wiring.", 5000);
    // Depending on requirements, you could halt or just disable SD logging.
    // For now, we continue, but offline logging will fail.
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
  pinMode(BUTTON_PIN1, INPUT); 
  pinMode(BUTTON_PIN2, INPUT);
}

/**
 * @brief Initializes the RTC and syncs it with an NTP server if online.
 */
void setupRtcAndSyncTime() {
  if (!rtc.begin()) {
    displayMessage("RTC Error!", "Check wiring.", 5000);
    while (1) { delay(1); } // Halt on critical error
  }
  
  if (rtc.lostPower()) {
    displayMessage("RTC Power Lost!", "Syncing time...", 2000);
    // If WiFi is connected later, time will be synced. If not, it will use the time it was compiled.
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
}

/**
 * @brief Displays a two-line message on the LCD. Clears the screen first.
 * @param line1 Text for the first row.
 * @param line2 Text for the second row (optional).
 * @param delayMs Duration to show the message before returning (optional).
 */
void displayMessage(String line1, String line2, int delayMs) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  if (delayMs > 0) {
    unsigned long startTime = millis();
    while (millis() - startTime < delayMs) {
      // السماح بتشغيل مهام أخرى خفيفة أثناء الانتظار
      yield(); 
    }
  }
}

void showCurrentTimeAndAnimation() {
  int currentHour = client.getHours();
  int currentMinute = client.getMinutes();
  int currentSecond = client.getSeconds();

  // *** تنسيق الساعة 12 ساعة وتوسيطها ***
  String timeString;
  String ampm = "";

  // تحويل الساعة من تنسيق 24 ساعة إلى 12 ساعة وتحديد AM/PM
  if (currentHour >= 12) {
    ampm = " PM";
    if (currentHour > 12) {
      currentHour -= 12;
    }
  } else {
    ampm = " AM";
    if (currentHour == 0) { // لو الساعة 00 (منتصف الليل) تتحول لـ 12 صباحاً
      currentHour = 12;
    }
  }

  // بناء سلسلة الوقت بالتنسيق HH:MM:SS AM/PM
  timeString = (currentHour < 10 ? "0" : "") + String(currentHour) + ":" +
               (currentMinute < 10 ? "0" : "") + String(currentMinute) + ":" +
               (currentSecond < 10 ? "0" : "") + String(currentSecond) + ampm;

  // لحساب مكان الساعة في المنتصف (16 حرف عرض الشاشة)
  int stringLength = timeString.length();
  int startColumn = (16 - stringLength) / 2; // (عرض الشاشة - طول السلسلة) / 2
  lcd.clear();
  lcd.setCursor(startColumn, 0); // في منتصف السطر الأول
  lcd.print(timeString);

  // تحديث وعرض الرسوم المتحركة
  if (millis() - lastAnimationUpdateTime > ANIMATION_FRAME_DURATION) {
    lcd.setCursor(15, 0); // في نهاية السطر الأول
    switch (currentAnimationFrame) {
      case 0:
        lcd.write((uint8_t)0); // عرض الابتسامة (رقم 0)
        break;
      case 1:
        lcd.write((uint8_t)1); // عرض العبوس (رقم 1)
        break;
      case 2:
        lcd.write((uint8_t)2); // عرض الأذرع لأسفل (رقم 2)
        break;
      case 3:
        lcd.write((uint8_t)3); // عرض الأذرع لأعلى (رقم 3)
        break;
    }
    
    currentAnimationFrame++;
    if (currentAnimationFrame >= TOTAL_ANIMATION_FRAMES) { // لو وصلت لآخر إطار
      currentAnimationFrame = 0; // ارجع لأول إطار
    }
    lastAnimationUpdateTime = millis();
  }
  lcd.setCursor(0, 1); 
  lcd.print("Press BTN to scan ");
}

/**
 * @brief Enters light sleep to save power and configures buttons to wake the ESP32.
 */
void enterLightSleep() {
    displayMessage("Entering sleep...", "Press Btn to wake", 2000);
    lcd.noDisplay(); // Turn off LCD backlight if controlled by a transistor

    // Enable wakeup from either button (active LOW)
    //esp_sleep_enable_ext1_wakeup(GPIO_NUM_34, 0); 
   // esp_sleep_enable_ext1_wakeup(GPIO_NUM_35, 0); 
    esp_sleep_enable_ext1_wakeup((1ULL << BUTTON_PIN1) | (1ULL << BUTTON_PIN2), ESP_EXT1_WAKEUP_ALL_LOW); // This might need adjustment depending on your button circuit (pull-up vs pull-down)
    esp_light_sleep_start();

    // --- Code execution resumes here after wakeup ---
    lcd.display();
    //lastActivityTime = millis(); // Reset activity timer upon waking
    //showMainMenu();
}

void updateActivityTime() {
    lastActivityTime = millis();
}


/**************************************************************************************************
 *                                   Button Handling Logic                                        *
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
 *                                   WiFi & Server Functions                                      *
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
  updateActivityTime();
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
bool syncAttendanceToServer(uint16_t id, time_t timestamp) {
  if (WiFi.status() != WL_CONNECTED) return false;

  StaticJsonDocument<128> doc;
  doc["id"] = id;
  doc["timestamp"] = timestamp;

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
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    String url = String(SERVER_HOST) + endpoint;
    http.begin(url); 
    http.addHeader("Content-Type", "application/json");

    int httpCode = http.POST(payload);
    http.end();

    return (httpCode >= 200 && httpCode < 300); // Return true for any 2xx success codes
}


/**************************************************************************************************
 *                                Fingerprint Operation Functions                                 *
 **************************************************************************************************/


/**
 * @brief Continuously scans for a fingerprint and logs attendance if a match is found.
 */
void scanForFingerprint() {
  // Wait for a finger to be placed on the sensor
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return;

  // Convert the image to a feature template
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return;

  // Search the sensor's internal database for a match
  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) {
    updateActivityTime();
    uint16_t fingerID = finger.fingerID;
    displayMessage("Welcome!", "ID: " + String(fingerID), 2000);
    
    // Log the attendance (this function handles online/offline logic)
    DateTime now = rtc.now();
    if(syncAttendanceToServer(fingerID, now.unixtime())){
        displayMessage("Attendance Sent!", "", 1500);
    } else {
        displayMessage("Saved Offline", "", 1500);
        logAttendanceOffline(fingerID, now.unixtime());
    }
    showMainMenu();
  } else if (p == FINGERPRINT_NOTFOUND) { // <-- ده التعديل اللي هنضيفه
    displayMessage("Fingerprint", "Not Found!", 1500); // <-- لعرض رسالة "Not Found"
    delay(500); //Small delay to prevent rapid failed reads if a finger is held down
    showMainMenu();
  }
  else {
    // Other errors or states
    displayMessage("Scan Error", "Try Again!", 1500); // ممكن نعرض رسالة عامة للأخطاء الأخر
    showMainMenu();
  }
}

/**
 * @brief Manages the multi-step process of enrolling a new user.
 */
void enrollNewFingerprint() {
  displayMessage("Enrollment:", "Checking server...", 0); // رسالة توضح أننا بنتحقق من السيرفر

  uint16_t newId = 0; // هتكون 0 لو مفيش ID صالح من السيرفر

  // 1. تحقق من اتصال الواي فاي أولاً
  if (WiFi.status() != WL_CONNECTED) {
    displayMessage("No WiFi!", "Cannot enroll.", 2000);
    showMainMenu();
    return; // إيقاف العملية لو مفيش إنترنت
  }

  // 2. لو فيه إنترنت، حاول تجيب الـ ID من السيرفر
  displayMessage("Enrollment:", "Fetching ID...", 0); // رسالة أثناء جلب ال ID
  newId = fetchLastIdFromServer() +1;
  /*
  if (newId > finger.templateCount) {
      displayMessage("server Error", "ID Invalid", 2000);
      showMainMenu();
      return;
  }*/
  updateActivityTime();
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

  displayMessage("Creating model", "Please wait...",1000);

    StaticJsonDocument<64> doc;
    doc["id"] = newId;
    String payload;
    serializeJson(doc, payload);
    
  displayMessage("Syncing to Server", "Please wait...", 0);
  if (WiFi.status() == WL_CONNECTED && logToServer("/api/SensorData", payload)) {
    if (createAndStoreModel(newId) == FINGERPRINT_OK) {
      displayMessage("Enrolled!", "ID: " + String(newId), 2000);
      displayMessage("Synced to Server", "", 2000);
    } else {
      displayMessage("Failed storing", "Try again", 2000);
    }
  } else {
    displayMessage("Server Sync Failed", "Enrollment Blocked", 3000);
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
    if (digitalRead(BUTTON_PIN1) == LOW) 
    updateActivityTime();
    return FINGERPRINT_ENROLLMISMATCH;
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
 *                               SD Card and Offline Logging                                      *
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

      if (syncAttendanceToServer(id, timestamp)) {
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
 *                                          Menu Actions                                          *
 **************************************************************************************************/


/**
 * @brief Shows the options menu on the LCD.
 */
void showOptionsMenu() {
  currentMenuState = MenuState::OPTIONS_MENU;
  displayMessage("Hold btn1: Clear", "Timeout: 10s");
  updateActivityTime(); // Reset timer for menu timeout
}

/**
 * @brief Shows the main menu on the LCD.
 */
void showMainMenu() {
  currentMenuState = MenuState::MAIN_MENU;
  displayMessage("btn1:add finger", "btn2:manage wifi ");
  updateActivityTime();
}

/**
 * @brief Contacts the server for authorization and then deletes all fingerprint data.
 */
void attemptToClearAllData() {
  displayMessage("Authorizing...", "Please wait", 0);
  
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
  updateActivityTime();
}
