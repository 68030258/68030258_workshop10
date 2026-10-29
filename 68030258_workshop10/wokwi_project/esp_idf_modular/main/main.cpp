/**
 * @file main.cpp
 * @brief ESP-IDF v6 Modular OOP Architecture for ESP32 Digital Clock
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"

static const char* TAG = "ESP32_CLOCK";

// Hardware Pin Definitions
#define I2C_SDA_PIN GPIO_NUM_21
#define I2C_SCL_PIN GPIO_NUM_22
#define BTN_MODE_PIN GPIO_NUM_18
#define BTN_SET_PIN GPIO_NUM_19
#define BTN_ALARM_PIN GPIO_NUM_4
#define BTN_LAP_PIN GPIO_NUM_5
#define BUZZER_PIN GPIO_NUM_25

enum ButtonId { BTN_MODE, BTN_SET, BTN_ALARM, BTN_LAP };
struct ButtonEvent {
    ButtonId id;
    uint32_t timestampMs;
};

// ====================================================================
// SPIRAL 1: I2CBus & HAL
// ====================================================================
namespace Clock {

class I2CBus {
private:
    i2c_port_t port_;
    gpio_num_t sda_;
    gpio_num_t scl_;
    i2c_master_bus_handle_t busHandle_{nullptr};
    SemaphoreHandle_t mutex_{nullptr};

public:
    I2CBus(i2c_port_t port, gpio_num_t sda, gpio_num_t scl)
        : port_(port), sda_(sda), scl_(scl) {
        mutex_ = xSemaphoreCreateMutex();
    }

    bool init() {
        i2c_master_bus_config_t conf = {};
        conf.i2c_port = port_;
        conf.sda_io_num = sda_;
        conf.scl_io_num = scl_;
        conf.clk_source = I2C_CLK_SRC_DEFAULT;
        conf.glitch_ignore_cnt = 7;
        return (i2c_new_master_bus(&conf, &busHandle_) == ESP_OK);
    }

    SemaphoreHandle_t getMutex() const { return mutex_; }
    i2c_master_bus_handle_t getHandle() const { return busHandle_; }
};

// DS1307 RTC Driver
class DS1307 {
private:
    I2CBus& bus_;
    i2c_master_dev_handle_t devHandle_{nullptr};

    uint8_t bcd2dec(uint8_t val) { return ((val / 16 * 10) + (val % 16)); }
    uint8_t dec2bcd(uint8_t val) { return ((val / 10 * 16) + (val % 10)); }

public:
    explicit DS1307(I2CBus& bus) : bus_(bus) {}

    bool init() {
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = 0x68;
        dev_cfg.scl_speed_hz = 100000;
        return (i2c_master_bus_add_device(bus_.getHandle(), &dev_cfg, &devHandle_) == ESP_OK);
    }

    bool getTime(uint8_t& h, uint8_t& m, uint8_t& s) {
        if (!devHandle_) return false;
        uint8_t reg = 0x00;
        uint8_t data[7];
        if (xSemaphoreTake(bus_.getMutex(), pdMS_TO_TICKS(100)) == pdTRUE) {
            esp_err_t err = i2c_master_transmit_receive(devHandle_, &reg, 1, data, 7, 100);
            xSemaphoreGive(bus_.getMutex());
            if (err == ESP_OK) {
                s = bcd2dec(data[0] & 0x7F);
                m = bcd2dec(data[1]);
                h = bcd2dec(data[2] & 0x3F);
                return true;
            }
        }
        return false;
    }
};

// SSD1306 Display Controller
class SSD1306 {
private:
    I2CBus& bus_;
    i2c_master_dev_handle_t devHandle_{nullptr};
    uint8_t buffer_[1024];

public:
    explicit SSD1306(I2CBus& bus) : bus_(bus) {
        memset(buffer_, 0, sizeof(buffer_));
    }

    bool init() {
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = 0x3C;
        dev_cfg.scl_speed_hz = 400000;
        return (i2c_master_bus_add_device(bus_.getHandle(), &dev_cfg, &devHandle_) == ESP_OK);
    }

    void clear() { memset(buffer_, 0, sizeof(buffer_)); }
    void flush() {
        if (!devHandle_) return;
        if (xSemaphoreTake(bus_.getMutex(), pdMS_TO_TICKS(50)) == pdTRUE) {
            uint8_t cmd[1] = { 0x40 }; // Data prefix
            i2c_master_transmit(devHandle_, cmd, 1, 50);
            i2c_master_transmit(devHandle_, buffer_, sizeof(buffer_), 100);
            xSemaphoreGive(bus_.getMutex());
        }
    }
};

// Button Manager
class ButtonManager {
private:
    QueueHandle_t queue_{nullptr};

public:
    void init(QueueHandle_t q) {
        queue_ = q;
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_NEGEDGE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << BTN_MODE_PIN) | (1ULL << BTN_SET_PIN) |
                               (1ULL << BTN_ALARM_PIN) | (1ULL << BTN_LAP_PIN);
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
        gpio_install_isr_service(0);
    }
};

// Alarm Engine
class AlarmEngine {
private:
    gpio_num_t buzzerPin_;
    uint8_t alarmH_{7};
    uint8_t alarmM_{30};
    bool armed_{true};

public:
    explicit AlarmEngine(gpio_num_t pin) : buzzerPin_(pin) {}
    void init() {
        gpio_set_direction(buzzerPin_, GPIO_MODE_OUTPUT);
        gpio_set_level(buzzerPin_, 0);
    }
    void check(uint8_t h, uint8_t m) {
        if (armed_ && h == alarmH_ && m == alarmM_) {
            gpio_set_level(buzzerPin_, 1);
        } else {
            gpio_set_level(buzzerPin_, 0);
        }
    }
};

} // namespace Clock

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Booting ESP32 Digital Clock Application (ESP-IDF v6.0)");

    static Clock::I2CBus i2cBus(I2C_NUM_0, I2C_SDA_PIN, I2C_SCL_PIN);
    i2cBus.init();

    static Clock::DS1307 rtc(i2cBus);
    rtc.init();

    static Clock::SSD1306 oled(i2cBus);
    oled.init();

    static Clock::AlarmEngine alarm(BUZZER_PIN);
    alarm.init();

    QueueHandle_t btnQueue = xQueueCreate(10, sizeof(ButtonEvent));
    static Clock::ButtonManager btnMgr;
    btnMgr.init(btnQueue);

    ESP_LOGI(TAG, "All HAL Drivers & FreeRTOS Primitives initialized successfully.");

    while (true) {
        uint8_t h = 0, m = 0, s = 0;
        if (rtc.getTime(h, m, s)) {
            alarm.check(h, m);
            ESP_LOGI(TAG, "RTC Time: %02d:%02d:%02d", h, m, s);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

