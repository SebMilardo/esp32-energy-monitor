#include <WiFi.h>
#include <WiFiClient.h>
#include "FS.h"
#include "SD_MMC.h"
#include "time.h"
#include "driver/rtc_io.h"
#include <Arduino.h>
#include <ESP32Time.h>
#include <UniversalTelegramBot.h>
#include <WiFiClientSecure.h>


#define BOTtoken "REPLACE_ME"
#define CHAT_ID "REPLACE_ME"
#define SSID "REPLACE_ME"
#define PASSWORD "REPLACE_ME"

#define DBG_OUTPUT_PORT Serial
#define uS_TO_S_FACTOR 1000000ULL /* Conversion factor for micro seconds to seconds */
#define INTERVAL 3600 * 2
#define NTP_TRY 10
#define DEBUG 0
#define FILENAME "/data.csv"

ESP32Time rtc;
RTC_DATA_ATTR unsigned long lastSync = 0;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 0;

static bool hasSD = false;

int print_GPIO_wake_up() {
  uint64_t GPIO_reason = esp_sleep_get_ext1_wakeup_status();
  int pin = (log(GPIO_reason)) / log(2);

#if DEBUG
  DBG_OUTPUT_PORT.print("GPIO that triggered the wake up: GPIO ");
  DBG_OUTPUT_PORT.println(pin);
#endif

  return pin;
}

bool setupSD() {
  bool sdOk = false;
  for (int i = 0; i < NTP_TRY * 100; i++) {
    if (SD_MMC.begin("/sdcard", true)) {
      sdOk = true;
      break;
    }
    delay(10);
  }

  if (!sdOk) {
#if DEBUG
    DBG_OUTPUT_PORT.println("SD Card Mount Failed");
#endif
    return false;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
#if DEBUG
    DBG_OUTPUT_PORT.println("No SD Card attached");
#endif
    return false;
  }
  return true;
}

void appendFile(fs::FS& fs, const char* path, const char* message) {
  File file = fs.open(path, FILE_APPEND);
  if (!file) {
#if DEBUG
    DBG_OUTPUT_PORT.println("Failed to open file for appending");
#endif
    return;
  }
  if (!file.print(message)) {
#if DEBUG
    DBG_OUTPUT_PORT.println("Append failed");
#endif
  }
}

void writeFile(fs::FS& fs, const char* path, const char* message) {
  File file = fs.open(path, FILE_WRITE);
  if (!file) {
#if DEBUG
    DBG_OUTPUT_PORT.println("Failed to open file for writing");
#endif
    return;
  }
  if (!file.print(message)) {
#if DEBUG
    DBG_OUTPUT_PORT.println("Write failed");
#endif
  }
}

File myFile;
bool isMoreDataAvailable();
byte getNextByte();


bool isMoreDataAvailable() {
  return myFile.available();
}

byte getNextByte() {
  return myFile.read();
}

bool sendFile(fs::FS& fs, const char* path) {
  bool result = false;
  myFile = fs.open(path, FILE_READ);
  if (myFile) {
    int size = myFile.size();
    WiFiClientSecure client;
    client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
    UniversalTelegramBot bot(BOTtoken, client);
    String sent = bot.sendMultipartFormDataToTelegram("sendDocument", "document", rtc.getTime("%Y-%m-%d %H") + ".csv", "text/plain", CHAT_ID, size,
                                                      isMoreDataAvailable, getNextByte, nullptr, nullptr);
    if (sent) {
      result = true;
    } else {
#if DEBUG
      DBG_OUTPUT_PORT.println("File was not sent");
#endif
    }
    myFile.close();
  } else {
#if DEBUG
    DBG_OUTPUT_PORT.println("Cannot read file");
#endif
  }
  return result;
}

bool connectToWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);

  // Wait for connection
  uint8_t i = 0;
  while (WiFi.status() != WL_CONNECTED && i++ < 60) {  //wait 30 seconds
    delay(500);
  }
  if (i == 61) {
#if DEBUG
    DBG_OUTPUT_PORT.print("Could not connect to ");
    DBG_OUTPUT_PORT.println(SSID);
#endif
    return false;
  }
#if DEBUG
  DBG_OUTPUT_PORT.print("Connected! IP address: ");
  DBG_OUTPUT_PORT.println(WiFi.localIP());
#endif
  return true;
}

time_t getNtpTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  struct tm timeinfo;
  for (int i = 0; i < NTP_TRY; i++) {
    if (getLocalTime(&timeinfo)) {
#if DEBUG
      DBG_OUTPUT_PORT.print("Current time: ");
      DBG_OUTPUT_PORT.println(rtc.getTime("%Y-%m-%d %H:%M:%S"));
#endif
      rtc.setTimeStruct(timeinfo);
      return mktime(&timeinfo);
    }
    delay(500);
  }
#if DEBUG
  DBG_OUTPUT_PORT.println("Failed to obtain time");
#endif
  return -1;
}

void append_time(int gpio) {
  digitalWrite(33, LOW);
  // Writing to sd card
  appendFile(SD_MMC, FILENAME, (rtc.getTime("%Y-%m-%d %H:%M:%S") + "," + String(gpio) + "\n").c_str());
  digitalWrite(33, HIGH);
}

void goToSleep() {
  pinMode(GPIO_NUM_15, INPUT_PULLDOWN);
  rtc_gpio_hold_en(GPIO_NUM_15);
  pinMode(GPIO_NUM_2, INPUT_PULLDOWN);
  rtc_gpio_hold_en(GPIO_NUM_2);

  esp_sleep_enable_ext1_wakeup(GPIO_SEL_12 | GPIO_SEL_15 | GPIO_SEL_2, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

void wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason;

  int pin = 0;
  wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
    pin = print_GPIO_wake_up();

    // setup SD card
    if (setupSD()) {
      if (pin == GPIO_NUM_15 | pin == GPIO_NUM_12) {
        append_time(pin);
      } else if (pin == GPIO_NUM_2) {
        // send data to Telegram
        bool connected = false;
        if (WiFi.status() != WL_CONNECTED) {
          connected = connectToWifi();
        }
        if (connected) {
          getNtpTime();
          sendFile(SD_MMC, FILENAME);
        }
      } else {
        append_time(GPIO_NUM_12);
        append_time(GPIO_NUM_15);
      }
    }
  }
}

void setup() {
  pinMode(GPIO_NUM_15, OUTPUT);
  pinMode(GPIO_NUM_2, OUTPUT);
  pinMode(33, OUTPUT);

  // put your setup code here, to run once:
#if DEBUG
  DBG_OUTPUT_PORT.begin(115200);
  DBG_OUTPUT_PORT.setDebugOutput(true);
#endif

  wakeup_reason();

  if (((rtc.getEpoch() - lastSync) > INTERVAL) || lastSync == 0) {
    // Init and get the time
    bool connected = connectToWifi();
    if (connected) {
      getNtpTime();
    }

    if (lastSync == 0) {
      // setup SD card
      if (setupSD()) {
        if (!SD_MMC.exists(FILENAME)) {
          writeFile(SD_MMC, FILENAME, "Timestamp,Data\n");
        }
      }
      if (connected) {
        WiFiClientSecure client;
        client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
        UniversalTelegramBot bot(BOTtoken, client);
        bot.sendMessage(CHAT_ID, "ESP32-CAM Online", "");
      }
    } else {
      if (connected) {
        if (sendFile(SD_MMC, FILENAME)) {
          writeFile(SD_MMC, FILENAME, "Timestamp,Data\n");
        }
      }
    }

    lastSync = rtc.getEpoch();
  }

  goToSleep();
}

void loop() {
}
