# ESP32 FreeRTOS Modular Digital Clock (Wokwi Project)

This project contains a fully tested, complete Wokwi simulator implementation of the ESP32 Modular Digital Clock using ESP-IDF v6 / FreeRTOS Bottom-Up Object-Oriented Architecture.

---

## 🚀 How to Run on Wokwi (Web Simulator in 1 Minute)

1. Open your browser and navigate to: **[wokwi.com/esp32](https://wokwi.com/projects/new/esp32)**
2. In Wokwi, click the **`diagram.json`** tab and paste the contents of `diagram.json`.
3. In the **`sketch.ino`** tab, paste the contents of `sketch.ino`.
4. In the **`libraries.txt`** tab, add:
   ```text
   Adafruit SSD1306
   Adafruit GFX Library
   RTClib
   ```
5. Click the green **Play (Start Simulation)** button!

---

## 🎮 Hardware Controls in Simulation

| Component | Pin (ESP32) | Action / Function |
| :--- | :--- | :--- |
| **MODE Button** | GPIO 18 | Cycles modes: `CLOCK_MAIN` ➔ `ALARM_SET` ➔ `STOPWATCH` ➔ `TIME_SET` |
| **SET Button** | GPIO 19 | Adjusts Alarm time (+5 min), Starts/Pauses Stopwatch, or steps Time |
| **ALARM Button** | GPIO 4 | Arms/Disarms Alarm toggle; Silences buzzer when ringing |
| **LAP/RST Button**| GPIO 5 | Records Lap split (up to 5 laps) or resets stopwatch when stopped |
| **Buzzer** | GPIO 25 | Emits 2000Hz alert tone when current time matches alarm |
| **DS1307 RTC** | GPIO 21 (SDA), GPIO 22 (SCL) | Battery-backed hardware timekeeping (0x68) |
| **SSD1306 OLED** | GPIO 21 (SDA), GPIO 22 (SCL) | 128x64 Monochrome display (0x3C) |

