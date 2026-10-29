/**
 * @file sketch.ino
 * @brief ESP32 FreeRTOS Modular OOP Digital Clock for Wokwi Simulator
 * @details Complete implementation fixing all placeholder stubs from Suite & Dashboard.
 *          Target: Wokwi ESP32 Web Simulator (Instant 1-Click Execution)
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>
#include <WiFi.h>
#include <time.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

// Pinout Definitions (Aligned with diagram.json)
#define PIN_SDA 21
#define PIN_SCL 22
#define PIN_BTN_MODE 18
#define PIN_BTN_SET 19
#define PIN_BTN_ALARM 4
#define PIN_BTN_LAP 5
#define PIN_BUZZER 25

// Network Credentials
const char* WIFI_SSID = "Wokwi-GUEST"; // Wokwi simulated Wi-Fi
const char* WIFI_PASS = "";
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 7 * 3600; // GMT+7 Bangkok / Indochina
const int   DAYLIGHT_OFFSET_SEC = 0;

// System Clock Modes
enum DisplayMode {
    MODE_CLOCK_MAIN = 0,
    MODE_ALARM_SET,
    MODE_STOPWATCH,
    MODE_TIME_SET
};

// Button Events
enum ButtonId { BTN_MODE, BTN_SET, BTN_ALARM, BTN_LAP };
struct ButtonEvent {
    ButtonId id;
    uint32_t timestamp;
};

// FreeRTOS Handles
static SemaphoreHandle_t xI2CMutex = NULL;
static QueueHandle_t xButtonQueue = NULL;

// ====================================================================
// SPIRAL 1: HARDWARE ABSTRACTION LAYER (HAL)
// ====================================================================
class HardwareDisplay {
private:
    Adafruit_SSD1306 display;

public:
    HardwareDisplay() : display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET) {}

    bool init() {
        if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
            Serial.println(F("[ERROR] SSD1306 allocation failed"));
            return false;
        }
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.display();
        return true;
    }

    void clear() { display.clearDisplay(); }
    void render() { display.display(); }

    void drawHeader(bool wifiOk, bool alarmArmed) {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.print(F("ESP32 CLOCK"));
        display.setCursor(75, 0);
        display.print(wifiOk ? F("NTP:OK") : F("NTP:--"));
        if (alarmArmed) {
            display.setCursor(118, 0);
            display.print(F("A"));
        }
        display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
    }

    void drawTime(const char* timeStr, const char* dateStr) {
        display.setTextSize(2);
        display.setCursor(16, 20);
        display.print(timeStr);

        display.setTextSize(1);
        display.setCursor(18, 48);
        display.print(dateStr);
    }

    void drawAlarmMenu(int hour, int min, bool armed) {
        display.setTextSize(1);
        display.setCursor(20, 16);
        display.print(F("=== SET ALARM ==="));

        display.setTextSize(2);
        display.setCursor(34, 30);
        char buf[10];
        snprintf(buf, sizeof(buf), "%02d:%02d", hour, min);
        display.print(buf);

        display.setTextSize(1);
        display.setCursor(22, 52);
        display.print(F("Status: "));
        display.print(armed ? F("ARMED") : F("DISABLED"));
    }

    void drawStopwatch(uint32_t totalMs, uint32_t laps[], size_t lapCount) {
        display.setTextSize(1);
        display.setCursor(15, 14);
        display.print(F("LAP STOPWATCH"));

        uint32_t m = totalMs / 60000;
        uint32_t s = (totalMs % 60000) / 1000;
        uint32_t cs = (totalMs % 1000) / 10;

        char buf[16];
        snprintf(buf, sizeof(buf), "%02u:%02u.%02u", m, s, cs);
        display.setTextSize(2);
        display.setCursor(18, 26);
        display.print(buf);

        display.setTextSize(1);
        display.setCursor(0, 46);
        display.print(F("Laps: "));
        display.print(lapCount);

        if (lapCount > 0) {
            uint32_t last = laps[lapCount - 1];
            snprintf(buf, sizeof(buf), "Last:%02u:%02u.%02u", last / 60000, (last % 60000) / 1000, (last % 1000) / 10);
            display.setCursor(0, 56);
            display.print(buf);
        }
    }
};

// ====================================================================
// SPIRAL 2: TIME SUBSYSTEM (DS1307 RTC + NTP)
// ====================================================================
class TimeManager {
private:
    RTC_DS1307 rtc;
    bool ntpSynced;
    bool rtcAvailable;
    DateTime cachedTime;

public:
    TimeManager() : ntpSynced(false), rtcAvailable(false) {}

    bool init() {
        if (!rtc.begin()) {
            Serial.println(F("[WARN] DS1307 RTC not responding, using internal RTC"));
            rtcAvailable = false;
        } else {
            rtcAvailable = true;
            if (!rtc.isrunning()) {
                rtc.adjust(DateTime(2026, 10, 29, 12, 0, 0));
            }
        }
        return true;
    }

    void syncNTP() {
        Serial.println(F("[NTP] Connecting to Wi-Fi for SNTP calibration..."));
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        int retry = 0;
        while (WiFi.status() != WL_CONNECTED && retry < 10) {
            vTaskDelay(pdMS_TO_TICKS(500));
            retry++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println(F("[NTP] Wi-Fi Connected! Fetching time from pool.ntp.org..."));
            configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 5000)) {
                ntpSynced = true;
                if (rtcAvailable && xI2CMutex != NULL) {
                    if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                                           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
                        xSemaphoreGive(xI2CMutex);
                        Serial.println(F("[NTP] Successfully synchronized DS1307 RTC registers with NTP!"));
                    }
                }
            }
        } else {
            Serial.println(F("[NTP] Wi-Fi connection timed out. Falling back to local RTC."));
        }
    }

    DateTime getTime() {
        if (rtcAvailable && xI2CMutex != NULL) {
            if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                cachedTime = rtc.now();
                xSemaphoreGive(xI2CMutex);
            }
        } else {
            struct tm ti;
            if (getLocalTime(&ti, 50)) {
                cachedTime = DateTime(ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec);
            }
        }
        return cachedTime;
    }

    void adjustMinutes(int delta) {
        DateTime cur = getTime();
        DateTime next(cur.unixtime() + (delta * 60));
        if (rtcAvailable && xI2CMutex != NULL) {
            if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                rtc.adjust(next);
                xSemaphoreGive(xI2CMutex);
            }
        }
    }

    bool isNtpOk() const { return ntpSynced; }
};

// ====================================================================
// SPIRAL 3: INPUT MANAGER (ISR Debounce & Queue Event Pump)
// ====================================================================
class ButtonManager {
public:
    static void isrMode()  { enqueueEvent(BTN_MODE); }
    static void isrSet()   { enqueueEvent(BTN_SET); }
    static void isrAlarm() { enqueueEvent(BTN_ALARM); }
    static void isrLap()   { enqueueEvent(BTN_LAP); }

    static void enqueueEvent(ButtonId id) {
        static uint32_t lastPress = 0;
        uint32_t now = millis();
        if (now - lastPress > 200) { // 200ms Debounce filter
            lastPress = now;
            ButtonEvent ev = { id, now };
            if (xButtonQueue != NULL) {
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                xQueueSendFromISR(xButtonQueue, &ev, &xHigherPriorityTaskWoken);
                if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
            }
        }
    }

    void init() {
        pinMode(PIN_BTN_MODE, INPUT_PULLUP);
        pinMode(PIN_BTN_SET, INPUT_PULLUP);
        pinMode(PIN_BTN_ALARM, INPUT_PULLUP);
        pinMode(PIN_BTN_LAP, INPUT_PULLUP);

        attachInterrupt(digitalPinToInterrupt(PIN_BTN_MODE),  isrMode,  FALLING);
        attachInterrupt(digitalPinToInterrupt(PIN_BTN_SET),   isrSet,   FALLING);
        attachInterrupt(digitalPinToInterrupt(PIN_BTN_ALARM), isrAlarm, FALLING);
        attachInterrupt(digitalPinToInterrupt(PIN_BTN_LAP),   isrLap,   FALLING);
    }
};

// ====================================================================
// SPIRAL 4: BUSINESS LOGIC ENGINES (Alarm & Stopwatch)
// ====================================================================
class AlarmEngine {
private:
    uint8_t pinBuzzer;
    int alarmHour;
    int alarmMinute;
    bool armed;
    bool ringing;

public:
    AlarmEngine(uint8_t pin) : pinBuzzer(pin), alarmHour(7), alarmMinute(30), armed(true), ringing(false) {}

    void init() {
        pinMode(pinBuzzer, OUTPUT);
        digitalWrite(pinBuzzer, LOW);
    }

    void toggleArmed() { armed = !armed; }
    void incrementTime() {
        alarmMinute = (alarmMinute + 5) % 60;
        if (alarmMinute == 0) alarmHour = (alarmHour + 1) % 24;
    }

    void check(uint8_t h, uint8_t m, uint8_t s) {
        if (armed && h == alarmHour && m == alarmMinute && s < 20) {
            ringing = true;
            // Generate alert tone (PWM Beep)
            tone(pinBuzzer, 2000, 150);
        } else {
            if (ringing) {
                noTone(pinBuzzer);
                ringing = false;
            }
        }
    }

    void stopAlarm() {
        ringing = false;
        noTone(pinBuzzer);
    }

    int getHour() const { return alarmHour; }
    int getMin() const { return alarmMinute; }
    bool isArmed() const { return armed; }
    bool isRinging() const { return ringing; }
};

class StopwatchEngine {
private:
    bool running;
    uint32_t startTime;
    uint32_t accumulated;
    uint32_t laps[5];
    size_t lapCount;

public:
    StopwatchEngine() : running(false), startTime(0), accumulated(0), lapCount(0) {}

    void toggle() {
        if (running) {
            accumulated += (millis() - startTime);
            running = false;
        } else {
            startTime = millis();
            running = true;
        }
    }

    void lapOrReset() {
        if (running) {
            if (lapCount < 5) {
                laps[lapCount++] = getElapsed();
            }
        } else {
            accumulated = 0;
            lapCount = 0;
        }
    }

    uint32_t getElapsed() const {
        return accumulated + (running ? (millis() - startTime) : 0);
    }

    size_t getLapCount() const { return lapCount; }
    uint32_t* getLaps() { return laps; }
};

// ====================================================================
// SPIRAL 5: FREERTOS TASK INTEGRATION & APPLICATION CONTROLLER
// ====================================================================
static HardwareDisplay gDisplay;
static TimeManager     gTimeMgr;
static ButtonManager   gButtonMgr;
static AlarmEngine     gAlarm(PIN_BUZZER);
static StopwatchEngine gStopwatch;
static DisplayMode     gCurrentMode = MODE_CLOCK_MAIN;

// Task 1: Periodic OLED Display Render (10 FPS)
void vTaskClockDisplay(void* pvParameters) {
    char timeStr[16];
    char dateStr[24];

    while (true) {
        DateTime now = gTimeMgr.getTime();
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
        snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d", now.year(), now.month(), now.day());

        if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            gDisplay.clear();
            gDisplay.drawHeader(gTimeMgr.isNtpOk(), gAlarm.isArmed());

            switch (gCurrentMode) {
                case MODE_CLOCK_MAIN:
                    gDisplay.drawTime(timeStr, dateStr);
                    break;
                case MODE_ALARM_SET:
                    gDisplay.drawAlarmMenu(gAlarm.getHour(), gAlarm.getMin(), gAlarm.isArmed());
                    break;
                case MODE_STOPWATCH:
                    gDisplay.drawStopwatch(gStopwatch.getElapsed(), gStopwatch.getLaps(), gStopwatch.getLapCount());
                    break;
                case MODE_TIME_SET:
                    gDisplay.drawTime(timeStr, "[SETTING TIME]");
                    break;
            }

            gDisplay.render();
            xSemaphoreGive(xI2CMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // 100ms = 10 FPS
    }
}

// Task 2: Button Event Dispatcher
void vTaskButtonHandler(void* pvParameters) {
    ButtonEvent ev;
    while (true) {
        if (xQueueReceive(xButtonQueue, &ev, portMAX_DELAY) == pdTRUE) {
            switch (ev.id) {
                case BTN_MODE:
                    gCurrentMode = (DisplayMode)((gCurrentMode + 1) % 4);
                    Serial.printf("[FSM] Mode Switched -> %d\n", gCurrentMode);
                    break;

                case BTN_SET:
                    if (gCurrentMode == MODE_ALARM_SET) {
                        gAlarm.incrementTime();
                    } else if (gCurrentMode == MODE_STOPWATCH) {
                        gStopwatch.toggle();
                    } else if (gCurrentMode == MODE_TIME_SET) {
                        gTimeMgr.adjustMinutes(1);
                    }
                    break;

                case BTN_ALARM:
                    gAlarm.toggleArmed();
                    if (gAlarm.isRinging()) gAlarm.stopAlarm();
                    Serial.printf("[ALARM] Armed State: %s\n", gAlarm.isArmed() ? "ON" : "OFF");
                    break;

                case BTN_LAP:
                    if (gCurrentMode == MODE_STOPWATCH) {
                        gStopwatch.lapOrReset();
                    } else if (gAlarm.isRinging()) {
                        gAlarm.stopAlarm();
                    }
                    break;
            }
        }
    }
}

// Task 3: 1Hz Alarm Evaluator
void vTaskAlarmCheck(void* pvParameters) {
    while (true) {
        DateTime now = gTimeMgr.getTime();
        gAlarm.check(now.hour(), now.minute(), now.second());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Task 4: Background NTP Sync
void vTaskNTPSync(void* pvParameters) {
    // Initial sync
    gTimeMgr.syncNTP();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(3600000)); // Repeat every 1 hour
        gTimeMgr.syncNTP();
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println(F("\n=============================================="));
    Serial.println(F("ESP32 FreeRTOS Modular Digital Clock Starting"));
    Serial.println(F("=============================================="));

    Wire.begin(PIN_SDA, PIN_SCL);

    xI2CMutex = xSemaphoreCreateMutex();
    xButtonQueue = xQueueCreate(10, sizeof(ButtonEvent));

    gDisplay.init();
    gTimeMgr.init();
    gButtonMgr.init();
    gAlarm.init();

    // Spawn FreeRTOS Multitasking Threads
    xTaskCreatePinnedToCore(vTaskClockDisplay,  "TaskDisplay", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(vTaskButtonHandler, "TaskButtons", 3072, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(vTaskAlarmCheck,    "TaskAlarm",   2048, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(vTaskNTPSync,       "TaskNTP",     4096, NULL, 1, NULL, 0);

    Serial.println(F("[FreeRTOS] All 4 Tasks Running on Dual Cores!"));
}

void loop() {
    // FreeRTOS scheduler handles all tasks; keep idle loop low priority
    vTaskDelay(pdMS_TO_TICKS(1000));
}

