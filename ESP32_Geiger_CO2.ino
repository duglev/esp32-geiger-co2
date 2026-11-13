#include <WiFi.h>
#include <Wire.h>
#include <SensirionI2cScd4x.h>
#include <ESP32ZabbixSender.h>
#include "DHT.h"
#include "secrets.h"

// --- НАСТРОЙКИ ZABBIX ---
#define ZABBIX_PORT 10051
#define ZABBIX_HOST_NAME "ESP32-CO2-Office"

// --- ПИНЫ ---
#define I2C_SDA 21
#define I2C_SCL 22
#define DHTPIN 4
#define DHTTYPE DHT22
#define WIFI_LED_PIN 2  

const int ledGreen  = 25;
const int ledYellow = 26;
const int ledRed    = 27;
const int BUZZER_PIN = 23;

// --- ДИНАМИЧЕСКИЕ ПАРАМЕТРЫ ЗУММЕРА (ВАШИ НАСТРОЙКИ) ---
int toneFreq = 6000;               // Частота по умолчанию (6000 Гц)
unsigned long clickDuration = 3;   // Длительность по умолчанию (3 мс)
const unsigned long MIN_INTERVAL_MS = 12;  
const float MAX_POISSON_FACTOR = 8.0f;    

unsigned long lastClickTime = 0;
unsigned long clickStartTime = 0;
unsigned long nextClickInterval = 1000; 
bool clicking = false;
int lastCO2ForBuzzer = 0; 

// --- ПЕРЕМЕННАЯ ДЛЯ ОТЛАДКИ ---
int simulatedCO2 = 0; 

// --- ОБЪЕКТЫ ---
ESP32ZabbixSender zSender;
SensirionI2cScd4x scd41;
DHT dht(DHTPIN, DHTTYPE);

// --- СТРУКТУРА ДЛЯ ПЕРЕДАЧИ ДАННЫХ ---
struct SensorData {
  uint16_t co2;
  float tempSCD;
  float humSCD;
  float tempDHT;
  float humDHT;
  uint8_t scdStatus; 
};

QueueHandle_t zabbixQueue;

// --- ФУНКЦИИ ЗУММЕРА ---
unsigned long co2ToInterval(int co2) {
  if (co2 < 1000) return 0;
  const float RATE_MIN = 0.5f;  
  const float RATE_MAX = 12.0f; 
  float rate;
  
  if (co2 < 1500) {
    float t = (co2 - 1000) / 500.0f; 
    rate = RATE_MIN * powf(RATE_MAX / RATE_MIN, t); 
  } else {
    rate = RATE_MAX + (co2 - 1500) * 0.08f; 
  }
  unsigned long ms = (unsigned long)(1000.0f / rate);
  if (ms < MIN_INTERVAL_MS) ms = MIN_INTERVAL_MS;
  return ms; 
}

unsigned long poissonInterval(unsigned long base) {
  float u = (float)random(1, 1000000) / 1000000.0f;
  float f = -logf(u);
  if (f > MAX_POISSON_FACTOR) f = MAX_POISSON_FACTOR;
  unsigned long r = (unsigned long)(base * f);
  return (r < MIN_INTERVAL_MS) ? MIN_INTERVAL_MS : r;
}

void handleBuzzer(int co2ppm) {
  unsigned long now = millis();
  unsigned long baseInterval = co2ToInterval(co2ppm);

  if (baseInterval == 0) {
    if (clicking) { noTone(BUZZER_PIN); clicking = false; }
    lastCO2ForBuzzer = 0;
    return;
  }

  if (lastCO2ForBuzzer == 0) {
    lastCO2ForBuzzer = co2ppm;
    lastClickTime = now;
    nextClickInterval = poissonInterval(baseInterval);
    return;
  }

  if (!clicking && co2ppm > lastCO2ForBuzzer + 150) {
    nextClickInterval = MIN_INTERVAL_MS; 
  }
  lastCO2ForBuzzer = co2ppm;

  if (clicking) {
    // Используем динамическую длительность
    if (now - clickStartTime >= clickDuration) {
      noTone(BUZZER_PIN);
      clicking = false;
      lastClickTime = clickStartTime;
      nextClickInterval = poissonInterval(baseInterval);
    }
    return;
  }

  if (now - lastClickTime >= nextClickInterval) {
    // Используем динамическую частоту
    tone(BUZZER_PIN, toneFreq);
    clicking = true;
    clickStartTime = now;
  }
}

// ==========================================
// ЗАДАЧА ДЛЯ ЯДРА 0 (WI-FI И ZABBIX)
// ==========================================
void zabbixTask(void * parameter) {
  zSender.Init(IPAddress(192, 168, 0, 77), ZABBIX_PORT, ZABBIX_HOST_NAME);
  SensorData dataToSend;
  
  unsigned long lastRetry = millis(); 
  bool connecting = false;
  const unsigned long WIFI_RETRY_MS = 10000;
  const unsigned long WIFI_TIMEOUT_MS = 8000;

  for(;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (connecting && (millis() - lastRetry > WIFI_TIMEOUT_MS)) connecting = false;
      if (!connecting && (millis() - lastRetry > WIFI_RETRY_MS)) {
        connecting = true;
        lastRetry = millis();
        WiFi.disconnect();
        WiFi.begin(ssid, password);
      }
      vTaskDelay(pdMS_TO_TICKS(500)); 
      continue;
    }

    if (connecting) connecting = false;

    if (xQueueReceive(zabbixQueue, &dataToSend, pdMS_TO_TICKS(1000)) == pdPASS) {
      
      zSender.ClearItem();
      zSender.AddItem("esp32.co2",  String(dataToSend.co2));
      zSender.AddItem("esp32.temp", String(dataToSend.tempSCD, 2));
      zSender.AddItem("esp32.hum",  String(dataToSend.humSCD, 2));
      zSender.AddItem("esp32.scd41.status", String(dataToSend.scdStatus));
      if (zSender.Send() == EXIT_FAILURE) {
        Serial.println("[Core 0] Ошибка отправки Пакета 1.");
      }

      if (!isnan(dataToSend.tempDHT) && !isnan(dataToSend.humDHT)) {
        zSender.ClearItem(); 
        zSender.AddItem("esp32.temp_dht", String(dataToSend.tempDHT, 2));
        zSender.AddItem("esp32.hum_dht",  String(dataToSend.humDHT, 2));
        if (zSender.Send() == EXIT_FAILURE) {
          Serial.println("[Core 0] Ошибка отправки Пакета 2 (DHT).");
        }
      }
    }
  }
}

// --- SETUP (Ядро 1) ---
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(ledGreen, OUTPUT); pinMode(ledYellow, OUTPUT); pinMode(ledRed, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT); pinMode(WIFI_LED_PIN, OUTPUT);
  digitalWrite(ledGreen, LOW); digitalWrite(ledYellow, LOW); digitalWrite(ledRed, LOW);
  digitalWrite(BUZZER_PIN, LOW); digitalWrite(WIFI_LED_PIN, LOW);

  randomSeed(esp_random()); 
  dht.begin();

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(5); 

  uint16_t error; char errorMessage[256];
  scd41.begin(Wire, SCD41_I2C_ADDR_62);
  scd41.stopPeriodicMeasurement(); delay(500);
  error = scd41.startPeriodicMeasurement();
  if (error) { Serial.println("Ошибка старта SCD41!"); } else { Serial.println("SCD41 OK!"); }

  WiFi.begin(ssid, password);

  zabbixQueue = xQueueCreate(1, sizeof(SensorData));
  if (zabbixQueue != NULL) {
    BaseType_t result = xTaskCreatePinnedToCore(
      zabbixTask, "ZabbixTask", 8192, NULL, 1, NULL, 0
    );
    if (result != pdPASS) {
      Serial.println("FATAL: Не удалось запустить ZabbixTask!");
    }
  } else {
    Serial.println("FATAL: Не удалось создать очередь Zabbix!");
  }

  Serial.println("=========================================");
  Serial.printf("ТЕКУЩИЕ НАСТРОЙКИ: Частота %d Гц, Длительность %lu мс\n", toneFreq, clickDuration);
  Serial.println("КОМАНДЫ ОТЛАДКИ:");
  Serial.println(" 1200   - симуляция CO2");
  Serial.println(" f=4000 - частота (Гц)");
  Serial.println(" d=2    - длительность (мс)");
  Serial.println(" 0      - вернуть реальный датчик");
  Serial.println("=========================================");
}

// --- LOOP (Ядро 1) ---
void loop() {
  unsigned long now = millis();
  
  static unsigned long lastDHTTime = 0; 
  static unsigned long lastSCDPollTime = 0;
  static unsigned long lastZabbixTime = 0;
  static unsigned long lastWifiBlinkTime = 0;
  static unsigned long lastValidSCDTime = 0; 
  static bool scdFault = false; 
  
  static float currentTemp = 0.0;
  static float currentHum  = 0.0;
  static uint16_t currentCO2 = 0;
  static float dhtTemp = NAN;
  static float dhtHum  = NAN;
  static uint8_t dhtErrors = 0;
  
  static String serialBuffer = ""; 

  // --- ОТЛАДКА: ЧТЕНИЕ КОМАНД ---
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        serialBuffer.trim();
        
        if (serialBuffer.startsWith("f=")) {
          int f = serialBuffer.substring(2).toInt();
          if (f >= 100 && f <= 10000) {
            toneFreq = f;
            Serial.printf(">>> Частота: %d Гц\n", toneFreq);
          } else {
            Serial.println(">>> Ошибка: частота от 100 до 10000 Гц");
          }
        } 
        else if (serialBuffer.startsWith("d=")) {
          int d = serialBuffer.substring(2).toInt();
          if (d >= 1 && d <= 50) {
            clickDuration = d;
            Serial.printf(">>> Длительность: %lu мс\n", clickDuration);
          } else {
            Serial.println(">>> Ошибка: длительность от 1 до 50 мс");
          }
        }
        else {
          // Обычное число (симуляция CO2)
          int val = serialBuffer.toInt();
          if (val > 0) { 
            simulatedCO2 = val; 
            Serial.printf(">>> Симуляция: %d ppm\n", val); 
          } else if (val == 0) { 
            simulatedCO2 = 0; 
            Serial.println(">>> Реальный датчик."); 
          }
        }
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
      if (serialBuffer.length() > 15) serialBuffer = ""; 
    }
  }

  // --- 0. ИНДИКАЦИЯ WI-FI ---
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(WIFI_LED_PIN, HIGH); 
  } else {
    if (now - lastWifiBlinkTime >= 500) {
      lastWifiBlinkTime = now;
      digitalWrite(WIFI_LED_PIN, !digitalRead(WIFI_LED_PIN));
    }
  }

  // --- 1. ЧТЕНИЕ DHT22 ---
  if (now - lastDHTTime >= 5000) {
    lastDHTTime = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    
    if (!isnan(t) && !isnan(h)) {
      dhtTemp = t;
      dhtHum = h;
      dhtErrors = 0;
    } else {
      dhtErrors++;
      if (dhtErrors > 20) { dhtTemp = NAN; dhtHum = NAN; }
    }
  }

  // --- 2. ЧТЕНИЕ SCD41 ---
  if (now - lastSCDPollTime >= 500) {
    lastSCDPollTime = now;
    uint16_t co2 = 0;
    float tSCD = 0, hSCD = 0;
    bool dataReady = false;
    uint16_t error;

    error = scd41.getDataReadyStatus(dataReady);
    if (!error && dataReady) {
      error = scd41.readMeasurement(co2, tSCD, hSCD);
      if (!error && co2 > 0) {
        currentCO2 = co2;
        currentTemp = tSCD; 
        currentHum = hSCD;  
        lastValidSCDTime = now; 
        scdFault = false; 
        
        if (currentCO2 < 1000) {
          digitalWrite(ledGreen, HIGH); digitalWrite(ledYellow, LOW); digitalWrite(ledRed, LOW);
        } else if (currentCO2 <= 1500) {
          digitalWrite(ledGreen, LOW); digitalWrite(ledYellow, HIGH); digitalWrite(ledRed, LOW);
        } else {
          digitalWrite(ledGreen, LOW); digitalWrite(ledYellow, LOW); digitalWrite(ledRed, HIGH);
        }
      }
    }
  }

  // --- 2.1 WATCHDOG SCD41 ---
  bool isTimeout = false;
  if (lastValidSCDTime > 0) {
    if (now - lastValidSCDTime > 15000) isTimeout = true;
  } else if (now > 15000) {
    isTimeout = true;
  }

  if (isTimeout && !scdFault) {
    scdFault = true;
    currentCO2 = 0; 
    Serial.println("SCD41: ТАЙМАУТ!");
    digitalWrite(ledGreen, LOW); digitalWrite(ledYellow, LOW); digitalWrite(ledRed, HIGH);
  }

  // --- 3. ЗУММЕР ---
  int co2ForBuzzer = (simulatedCO2 > 0) ? simulatedCO2 : currentCO2;
  handleBuzzer(co2ForBuzzer);

  // --- 4. ОТПРАВКА В ОЧЕРЕДЬ ---
  if (now - lastZabbixTime >= 5000) {
    lastZabbixTime = now;

    if (zabbixQueue != NULL) {
      SensorData newData;
      newData.co2 = currentCO2; 
      newData.tempSCD = currentTemp;
      newData.humSCD = currentHum;
      newData.tempDHT = dhtTemp;
      newData.humDHT = dhtHum;
      newData.scdStatus = scdFault ? 0 : 1; 
      
      xQueueOverwrite(zabbixQueue, &newData);
    }
  }
}