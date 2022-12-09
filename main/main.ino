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


#define BOTtoken "XXXXXXXXXXXXXXXXXX REPLACE ME XXXXXXXXXXXXXXXXXXXXXXX"
#define CHAT_ID  "XXXXXXXXXXXXXXXXXX REPLACE ME XXXXXXXXXXXXXXXXXXXXXXX"

#define DBG_OUTPUT_PORT Serial

#define uS_TO_S_FACTOR 1000000ULL /* Conversion factor for micro seconds to seconds */
#define INTERVAL 60 * 60 * 12
#define NTP_TRY 5

WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);
ESP32Time rtc;
RTC_DATA_ATTR unsigned long lastSync = 0;

const char* ssid = "XXXXXXXXXXXXXXXXXX REPLACE ME XXXXXXXXXXXXXXXXXXXXXXX";
const char* password = "XXXXXXXXXXXXXXXXXX REPLACE ME XXXXXXXXXXXXXXXXXXXXXXX";
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 0;
const char* filename = "/data.csv";
const char* header = "Timestamp,Data\n";

static bool hasSD = false;

int print_GPIO_wake_up() {
  uint64_t GPIO_reason = esp_sleep_get_ext1_wakeup_status();
  DBG_OUTPUT_PORT.print("GPIO that triggered the wake up: GPIO ");
  int pin = (log(GPIO_reason)) / log(2);
  DBG_OUTPUT_PORT.println(pin);
  return pin;
}

void setupSD() {
  pinMode(GPIO_NUM_15, OUTPUT);
  digitalWrite(GPIO_NUM_15, LOW);
  pinMode(GPIO_NUM_2, OUTPUT);
  digitalWrite(GPIO_NUM_2, LOW);

  bool sdOk = false;
  for (int i = 0; i < NTP_TRY * 100; i++) {
    if (SD_MMC.begin("/sdcard", true)) {
      sdOk = true;      
      break;
    }
    delay(10);
  }

  if (!sdOk){ 
    DBG_OUTPUT_PORT.println("SD Card Mount Failed");
    while (1) {
      delay(1000);
    }
  }
  
  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    DBG_OUTPUT_PORT.println("No SD Card attached");
    while (1) {
      delay(500);
    }
  }
}

void appendFile(fs::FS& fs, const char* path, const char* message) {
  DBG_OUTPUT_PORT.println(message);
  File file = fs.open(path, FILE_APPEND);
  if (!file) {
    DBG_OUTPUT_PORT.println("Failed to open file for appending");
    return;
  }
  if (!file.print(message)) {
    DBG_OUTPUT_PORT.println("Append failed");
  }
}

void writeFile(fs::FS& fs, const char* path, const char* message) {
  File file = fs.open(path, FILE_WRITE);
  if (!file) {
    DBG_OUTPUT_PORT.println("Failed to open file for writing");
    return;
  }
  if (!file.print(message)) {
    DBG_OUTPUT_PORT.println("Write failed");
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
    String sent = bot.sendMultipartFormDataToTelegram("sendDocument", "document", rtc.getTime("%Y-%m-%d %H") + ".csv", "text/plain", CHAT_ID, size,
                                                      isMoreDataAvailable, getNextByte, nullptr, nullptr);
    if (sent) {
      result = true;
    } else {
      DBG_OUTPUT_PORT.println("File was not sent");
    }
    myFile.close();
  } else {
    DBG_OUTPUT_PORT.println("Cannot read file");
  }
  return result;
}

void connectToWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  DBG_OUTPUT_PORT.print("Connecting to ");
  DBG_OUTPUT_PORT.println(ssid);

  // Wait for connection
  uint8_t i = 0;
  while (WiFi.status() != WL_CONNECTED && i++ < 40) {  //wait 20 seconds
    DBG_OUTPUT_PORT.print(".");
    delay(500);
  }
  DBG_OUTPUT_PORT.println(".");
  if (i == 41) {
    DBG_OUTPUT_PORT.print("Could not connect to ");
    DBG_OUTPUT_PORT.println(ssid);
    while (1) {
      delay(500);
    }
  } else {
    DBG_OUTPUT_PORT.print("Connected! IP address: ");
    DBG_OUTPUT_PORT.println(WiFi.localIP());
  }
}

time_t getNtpTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  struct tm timeinfo;
  for (int i = 0; i < NTP_TRY; i++) {
    if (getLocalTime(&timeinfo)) {
      DBG_OUTPUT_PORT.print("Current time: ");
      DBG_OUTPUT_PORT.println(rtc.getTime("%Y-%m-%d %H:%M:%S"));
      rtc.setTimeStruct(timeinfo);
      return mktime(&timeinfo);
    }
    delay(500);
  }
  DBG_OUTPUT_PORT.println("Failed to obtain time");
  return -1;
}

void append_time(int gpio) {
  digitalWrite(33, LOW);
  // Writing to sd card
  appendFile(SD_MMC, filename, (rtc.getTime("%Y-%m-%d %H:%M:%S") + "," + String(gpio) + "\n").c_str());
  digitalWrite(33, HIGH);
}

void wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason;

  int pin = 0;
  wakeup_reason = esp_sleep_get_wakeup_cause();
  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      DBG_OUTPUT_PORT.println("Wakeup caused by external signal using RTC_IO");
      break;

    case ESP_SLEEP_WAKEUP_EXT1:
      DBG_OUTPUT_PORT.println("Wakeup caused by external signal using RTC_CNTL");
      pin = print_GPIO_wake_up();
      
      // setup SD card
      setupSD();
      if (pin == GPIO_NUM_15 | pin == GPIO_NUM_12) {
        append_time(pin);
      } else if (pin == GPIO_NUM_2) {
        // send data to Telegram
        connectToWifi();
        getNtpTime();
        sendFile(SD_MMC, filename);
      } else {
        append_time(GPIO_NUM_12);
        append_time(GPIO_NUM_15);
      }
      break;

    case ESP_SLEEP_WAKEUP_TIMER:
      DBG_OUTPUT_PORT.println("Wakeup caused by timer");
      break;

    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      DBG_OUTPUT_PORT.println("Wakeup caused by touchpad");
      break;

    case ESP_SLEEP_WAKEUP_ULP:
      DBG_OUTPUT_PORT.println("Wakeup caused by ULP program");
      break;

    default:
      DBG_OUTPUT_PORT.println("Wakeup was not caused by deep sleep");
      break;
  }
}

void setup() {
  // put your setup code here, to run once:
  DBG_OUTPUT_PORT.begin(115200);
  DBG_OUTPUT_PORT.setDebugOutput(true);

  pinMode(33, OUTPUT);

  wakeup_reason();

  if (((rtc.getEpoch() - lastSync) > INTERVAL) || lastSync == 0){
    // Init and get the time
    connectToWifi();
    getNtpTime();

    if (lastSync == 0){
       // setup SD card
      setupSD();
      writeFile(SD_MMC, filename, header);
      bot.sendMessage(CHAT_ID, "ESP32-CAM Online", "");
    } else {
      if (sendFile(SD_MMC, filename)){
        writeFile(SD_MMC, filename, header);
      }
    }

    lastSync = rtc.getEpoch();
  }

  pinMode(GPIO_NUM_15, INPUT_PULLDOWN);
  rtc_gpio_hold_en(GPIO_NUM_15);
  pinMode(GPIO_NUM_2, INPUT_PULLDOWN);
  rtc_gpio_hold_en(GPIO_NUM_2);

  esp_sleep_enable_ext1_wakeup(GPIO_SEL_12 | GPIO_SEL_15 | GPIO_SEL_2, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

void loop() {
}
