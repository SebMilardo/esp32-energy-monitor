# esp32-energy-monitor
Logs interrupts received on pin 12 and 15 then sends the data to a Telegram bot. This code has been tested on an ESP32-CAM board. 

What this code does:

- Uses NTP to get time
- Stores it
- Goes to deep sleep
- Wakes up if an interrupt arrives on RTC pins 12 or 15
- Stores timestamp and pin on SD-CARD
- Periodically sends the recorded data to the Telegram bot or when an interrupt is received on pin 2

To make this work you need the following libraries:

- ArduinoJson
- ESP32Time
- Universal-Arduino-Telegram-Bot