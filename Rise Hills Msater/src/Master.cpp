/*
 * ============================================================
 *  Master Controller  —  ESP32
 *  Sends commands to slave devices via ESP-NOW.
 *  Accepts commands via USB Serial and Bluetooth.
 *
 *  LED Slave IDs  : blue | red | yellow | orange
 *  Mech Slaves    : building | alley
 *
 *  LED examples:
 *    blue reffect 0 500 chase white 30 2
 *    red reffect 100 300 blink red 400 3
 *    reffect 0 1000 breath blue 50 8        (all LED devices)
 *    orange reffect 500 700 sparkle cyan 20 60 15
 *    yellow reffect 0 200 fill orange
 *
 *  Mechanical examples:
 *    building down
 *    building up
 *    building stop
 *    alley 1
 *    alley 2
 *
 *  NOTE: Before flashing, update buildingMAC and alleyMAC
 *        with actual values printed on slave Serial at boot.
 * ============================================================
 */

#include <esp_now.h>
#include <WiFi.h>
#include "BluetoothSerial.h"

BluetoothSerial SerialBT;

// ─── LED Device registry ──────────────────────────────────────
typedef struct {
    uint8_t     mac[6];
    const char* id;
} Device;

const Device ledDevices[] = {
    {{0x78, 0x1C, 0x3C, 0xA2, 0x8C, 0xFC}, "blue"},
    {{0x68, 0x25, 0xDD, 0x2E, 0xDF, 0x34}, "red"},
    {{0x68, 0x25, 0xDD, 0x2E, 0xBB, 0x84}, "yellow"},
    {{0x68, 0x25, 0xDD, 0x2E, 0xE4, 0x08}, "orange"},
};
const int LED_DEVICES_COUNT = sizeof(ledDevices) / sizeof(ledDevices[0]);

// ─── Mechanical Slave MACs ────────────────────────────────────
// TODO: Replace with actual MAC addresses.
//       Slave prints its MAC on Serial/Bluetooth at boot.
//       Example output: "ESP-NOW ready. MAC: AA:BB:CC:DD:EE:FF"
uint8_t buildingMAC[] = {0x68, 0x25, 0xDD, 0x2E, 0xD3, 0xC4}; // Building Layout
uint8_t alleyMAC[]    = {0x68, 0x25, 0xDD, 0x30, 0xFB, 0xF4}; // Alley Stepper

// ─── Building ESP-NOW packet ──────────────────────────────────
// Matches EspNowCmd struct in Building.cpp
typedef struct {
    uint8_t cmd;
    uint8_t reserved;
} BuildingCmd;

// ─── Prototypes ───────────────────────────────────────────────
void    processCommand(String cmd);
void    sendToLedDevice(const Device& dev, const String& payload);
void    sendToAllLedDevices(const String& payload);
void    sendToBuilding(uint8_t cmd);
void    sendToAlley(char cmd);
void    turnOffAll();
void    turnOffOthers(const String& excludeId);
Device* findLedDeviceById(const String& id);
bool    processLedDeviceCommand(String cmd);
bool    processBuildingCommand(const String& subcmd);
bool    processAlleyCommand(const String& subcmd);
void    registerPeer(const uint8_t* mac, const char* label);
void    printMac(const uint8_t* mac);
void    clearSerialInput();
void    printHelp();

// ─── Setup ────────────────────────────────────────────────────
void setup() {
    delay(2000);
    Serial.begin(115200);
    while (!Serial) delay(10);

    Serial.println("\n\n=== MASTER CONTROLLER INIT ===");
    Serial.println("Rise Hills Master v3.0");
    Serial.println("==============================\n");

    if (!SerialBT.begin("RiseHills_Master"))
        Serial.println("[ERROR] Bluetooth init failed!");
    else
        Serial.println("[OK] Bluetooth ready as 'RiseHills_Master'");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    Serial.println("Initializing ESP-NOW...");
    if (esp_now_init() != ESP_OK) { Serial.println("[ERROR] ESP-NOW init failed!"); return; }
    Serial.println("[OK] ESP-NOW ready");

    Serial.println("\nRegistering LED peers:");
    for (int i = 0; i < LED_DEVICES_COUNT; i++) {
        registerPeer(ledDevices[i].mac, ledDevices[i].id);
    }

    Serial.println("\nRegistering mechanical peers:");
    registerPeer(buildingMAC, "building");
    registerPeer(alleyMAC,    "alley");

    printHelp();
    clearSerialInput();
}

// ─── Loop ─────────────────────────────────────────────────────
void loop() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        if (cmd.length()) processCommand(cmd);
    }
    if (SerialBT.available()) {
        String cmd = SerialBT.readStringUntil('\n');
        cmd.trim();
        if (cmd.length()) { Serial.printf("[BT] %s\n", cmd.c_str()); processCommand(cmd); }
    }
}

// ─── Peer registration ────────────────────────────────────────
void registerPeer(const uint8_t* mac, const char* label) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) == ESP_OK) {
        Serial.printf("  [+] %-10s  MAC: ", label);
        printMac(mac); Serial.println();
    } else {
        Serial.printf("  [!] Failed to register: %s\n", label);
    }
}

// ─── Help ─────────────────────────────────────────────────────
void printHelp() {
    Serial.println("\n=== COMMAND REFERENCE ===");

    Serial.println("── LED Devices ───────────────────────────────");
    Serial.println("  Devices : blue | red | yellow | orange");
    Serial.println();
    Serial.println("  Global (all LED devices):");
    Serial.println("    off");
    Serial.println("    on [color]");
    Serial.println("    random");
    Serial.println("    brightness <0-255>");
    Serial.println("    fill <color>");
    Serial.println("    blink <color> <ms> <n>");
    Serial.println("    breath <color> <steps> <ms>");
    Serial.println("    wipe <color> <ms>");
    Serial.println("    chase <color> <tail> <ms>");
    Serial.println("    sparkle <color> <count> <ms> <iter>");
    Serial.println("    strobe <color> <onMs> <offMs> <times>");
    Serial.println("    sections <n>");
    Serial.println("    range <s> <e> [color]");
    Serial.println();
    Serial.println("  Range-scoped effects (reffect):");
    Serial.println("    reffect <s> <e> fill <color>");
    Serial.println("    reffect <s> <e> off");
    Serial.println("    reffect <s> <e> blink <color> <ms> <n>");
    Serial.println("    reffect <s> <e> breath <color> <steps> <ms>");
    Serial.println("    reffect <s> <e> wipe <color> <ms>");
    Serial.println("    reffect <s> <e> chase <color> <tail> <ms>");
    Serial.println("    reffect <s> <e> sparkle <color> <cnt> <ms> <iter>");
    Serial.println("    reffect <s> <e> strobe <color> <on> <off> <n>");
    Serial.println();
    Serial.println("  Device-specific: <device> <command>");
    Serial.println("    blue on red");
    Serial.println("    red reffect 0 500 chase white 30 2");
    Serial.println("    yellow solo       ← ON this, OFF rest");
    Serial.println();
    Serial.println("  Colors: white|red|green|blue|yellow|cyan|magenta|orange|purple|warm|off");
    Serial.println("          or r,g,b,w  e.g. 255,100,0,0");

    Serial.println();
    Serial.println("── Building Layout (lift) ────────────────────");
    Serial.println("  building down      — go DOWN  (M1→M3→M2)");
    Serial.println("  building up        — go UP    (M2→M3→M1)");
    Serial.println("  building stop      — EMERGENCY STOP");
    Serial.println("  Note: Motor params (M1/M2/M3/SPD/ACC/DLY)");
    Serial.println("        configured directly via BT 'Building Layout'");

    Serial.println();
    Serial.println("── Alley Stepper ─────────────────────────────");
    Serial.println("  alley 1            — move to POS 1 (убрать)");
    Serial.println("  alley 2            — move to POS 2 (выдвинуть)");
    Serial.println("  alley help         — print alley status/help");

    Serial.println();
    Serial.println("── System ────────────────────────────────────");
    Serial.println("  status             — list all registered devices");
    Serial.println("  help               — this text");
    Serial.println("=========================\n");
}

// ─── Utilities ────────────────────────────────────────────────
void printMac(const uint8_t* mac) {
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02X", mac[i]);
        if (i < 5) Serial.print(":");
    }
}

void clearSerialInput() { while (Serial.available()) { Serial.read(); delay(1); } }

Device* findLedDeviceById(const String& id) {
    for (int i = 0; i < LED_DEVICES_COUNT; i++)
        if (String(ledDevices[i].id).equalsIgnoreCase(id)) return (Device*)&ledDevices[i];
    return nullptr;
}

// ─── LED Send helpers ─────────────────────────────────────────
void sendToLedDevice(const Device& dev, const String& payload) {
    if (payload.length() == 0 || payload.length() > 240) {
        Serial.println("[ERROR] Payload length invalid"); return;
    }
    String full = String(dev.id) + ":" + payload;
    Serial.printf("  --> %-8s  %s\n", dev.id, payload.c_str());
    esp_err_t res = esp_now_send(dev.mac, (uint8_t*)full.c_str(), full.length());
    if (res != ESP_OK) Serial.printf("    [!] Send error: %d\n", res);
    delay(50);
}

void sendToAllLedDevices(const String& payload) {
    Serial.printf("[ALL LED] %s\n", payload.c_str());
    for (int i = 0; i < LED_DEVICES_COUNT; i++) sendToLedDevice(ledDevices[i], payload);
}

void turnOffAll() {
    Serial.println("[ALL] Turning off all LED devices");
    sendToAllLedDevices("off");
}

void turnOffOthers(const String& excludeId) {
    Serial.printf("[SOLO] Keeping '%s' ON, turning off others\n", excludeId.c_str());
    for (int i = 0; i < LED_DEVICES_COUNT; i++)
        if (!String(ledDevices[i].id).equalsIgnoreCase(excludeId))
            sendToLedDevice(ledDevices[i], "off");
}

// ─── Building Send ────────────────────────────────────────────
// Sends a binary EspNowCmd packet to the Building Layout slave.
// cmd: 0 = STOP, 1 = DOWN, 2 = UP
void sendToBuilding(uint8_t cmd) {
    BuildingCmd pkt;
    pkt.cmd      = cmd;
    pkt.reserved = 0;
    const char* label = (cmd == 0) ? "STOP" : (cmd == 1) ? "DOWN" : "UP";
    Serial.printf("  --> building  %s (cmd=%d)\n", label, cmd);
    esp_err_t res = esp_now_send(buildingMAC, (uint8_t*)&pkt, sizeof(pkt));
    if (res != ESP_OK) Serial.printf("    [!] Building send error: %d\n", res);
    delay(50);
}

// ─── Alley Send ───────────────────────────────────────────────
// Sends a single raw char to the Alley Stepper slave.
// cmd: '1' = POS1 (убрать), '2' = POS2 (выдвинуть), 'H' = help
void sendToAlley(char cmd) {
    Serial.printf("  --> alley     '%c'\n", cmd);
    esp_err_t res = esp_now_send(alleyMAC, (uint8_t*)&cmd, 1);
    if (res != ESP_OK) Serial.printf("    [!] Alley send error: %d\n", res);
    delay(50);
}

// ─── Range command helper ──────────────────────────────────────
// Send all ranges in a single ESP-NOW packet so the slave renders them
// all at once (one strip.show()). Splitting into per-range packets was
// causing drops: strip.show() on 3700px takes ~148ms but packets
// arrived every 50ms, so the 3rd+ packets were discarded by onDataRecv.
void sendRangeTokens(const Device& dev, const String& rangesStr) {
    sendToLedDevice(dev, "range " + rangesStr);
}

void sendRangeTokensAll(const String& rangesStr) {
    sendToAllLedDevices("range " + rangesStr);
}

// ─── Building command processor ───────────────────────────────
bool processBuildingCommand(const String& subcmd) {
    Serial.printf("\n>>> [DEVICE: building] %s\n", subcmd.c_str());
    if (subcmd.equalsIgnoreCase("down") || subcmd == "1") {
        sendToBuilding(1);
        return true;
    }
    if (subcmd.equalsIgnoreCase("up") || subcmd == "2") {
        sendToBuilding(2);
        return true;
    }
    if (subcmd.equalsIgnoreCase("stop") || subcmd == "0") {
        sendToBuilding(0);
        return true;
    }
    Serial.printf("[ERROR] Unknown building command: '%s'\n", subcmd.c_str());
    Serial.println("  Valid: building down | building up | building stop");
    return true; // consumed — prefix matched
}

// ─── Alley command processor ───────────────────────────────────
bool processAlleyCommand(const String& subcmd) {
    Serial.printf("\n>>> [DEVICE: alley] %s\n", subcmd.c_str());
    if (subcmd == "1") {
        sendToAlley('1');
        return true;
    }
    if (subcmd == "2") {
        sendToAlley('2');
        return true;
    }
    if (subcmd.equalsIgnoreCase("help") || subcmd == "H" || subcmd == "h" || subcmd == "?") {
        sendToAlley('H');
        return true;
    }
    Serial.printf("[ERROR] Unknown alley command: '%s'\n", subcmd.c_str());
    Serial.println("  Valid: alley 1 | alley 2 | alley help");
    return true; // consumed — prefix matched
}

// ─── LED Device-specific command processor ───────────────────
bool processLedDeviceCommand(String cmd) {
    cmd.trim();
    int sp = cmd.indexOf(' ');
    String devId  = (sp == -1) ? cmd          : cmd.substring(0, sp);
    String subcmd = (sp == -1) ? String("on") : cmd.substring(sp + 1);
    subcmd.trim();

    // Check mechanical devices first
    if (devId.equalsIgnoreCase("building")) return processBuildingCommand(subcmd);
    if (devId.equalsIgnoreCase("alley"))    return processAlleyCommand(subcmd);

    Device* dev = findLedDeviceById(devId);
    if (!dev) return false;

    Serial.printf("\n>>> [DEVICE: %s] %s\n", dev->id, subcmd.c_str());

    // solo
    if (subcmd == "solo") {
        turnOffOthers(String(dev->id));
        sendToLedDevice(*dev, "on");
        return true;
    }

    // range — clear first, then send tokens
    if (subcmd.startsWith("range ")) {
        String rangesStr = subcmd.substring(6); rangesStr.trim();
        sendToLedDevice(*dev, "off");
        delay(100);
        sendRangeTokens(*dev, rangesStr);
        return true;
    }

    // reffect — pass through directly
    if (subcmd.startsWith("reffect ")) {
        sendToLedDevice(*dev, subcmd);
        return true;
    }

    // everything else passes through
    sendToLedDevice(*dev, subcmd);
    return true;
}

// ─── Main command dispatcher ──────────────────────────────────
void processCommand(String originalCmd) {
    String cmd = originalCmd;
    cmd.trim();
    if (cmd.length() == 0) return;

    Serial.printf("\n========================================\n");
    Serial.printf("CMD: '%s'\n", cmd.c_str());
    Serial.printf("========================================\n");

    if (cmd == "help")   { printHelp(); return; }

    if (cmd == "status") {
        Serial.println("\n=== DEVICE LIST ===");
        Serial.println("  LED devices:");
        for (int i = 0; i < LED_DEVICES_COUNT; i++) {
            Serial.printf("    %-8s  MAC: ", ledDevices[i].id);
            printMac(ledDevices[i].mac); Serial.println();
        }
        Serial.print("  building    MAC: "); printMac(buildingMAC); Serial.println();
        Serial.print("  alley       MAC: "); printMac(alleyMAC);    Serial.println();
        Serial.println("===================\n");
        return;
    }

    // explicit "all <cmd>" — LED only
    if (cmd.startsWith("all ")) {
        sendToAllLedDevices(cmd.substring(4));
        return;
    }

    // device-specific? (handles building/alley too)
    if (processLedDeviceCommand(cmd)) return;

    // ── Global LED shortcuts ──────────────────────────────────
    if (cmd == "off")                          { turnOffAll(); return; }
    if (cmd == "allOn")                        { sendToAllLedDevices("on white"); return; }
    if (cmd == "on" || cmd.startsWith("on ")) { sendToAllLedDevices(cmd); return; }
    if (cmd == "random")                       { sendToAllLedDevices("random"); return; }

    if (cmd.startsWith("brightness ") ||
        cmd.startsWith("fill ")       ||
        cmd.startsWith("blink ")      ||
        cmd.startsWith("breath ")     ||
        cmd.startsWith("wipe ")       ||
        cmd.startsWith("chase ")      ||
        cmd.startsWith("sparkle ")    ||
        cmd.startsWith("strobe ")     ||
        cmd.startsWith("sections "))
    {
        sendToAllLedDevices(cmd);
        return;
    }

    // reffect global — all LED devices
    if (cmd.startsWith("reffect ")) {
        sendToAllLedDevices(cmd);
        return;
    }

    // range global — clear all first, then send tokens
    if (cmd.startsWith("range ")) {
        String rangesStr = cmd.substring(6); rangesStr.trim();
        turnOffAll();
        delay(150);
        sendRangeTokensAll(rangesStr);
        return;
    }

    Serial.printf("[ERROR] Unknown command: '%s'\n", originalCmd.c_str());
    Serial.println("Type 'help' for command list.");
}
