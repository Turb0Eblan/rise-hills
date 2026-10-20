#include "BluetoothSerial.h"
#include <AccelStepper.h>
#include <Adafruit_NeoPixel.h>
#include <esp_now.h>
#include <WiFi.h>

#define DIR_PIN         13
#define STEP_PIN        12
#define ENDSTOP_PIN     14
#define LED_PIN         27
#define NUM_LEDS        171

#define STEPS_TO_POS2   7500
#define EXTRA_STEPS     100

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRBW + NEO_KHZ800);
AccelStepper stepper(AccelStepper::DRIVER, STEP_PIN, DIR_PIN);
BluetoothSerial SerialBT;

enum Position { POS_UNKNOWN, POS_1, POS_2 };
Position currentPos = POS_UNKNOWN;

enum State {
  STATE_HOMING,
  STATE_IDLE,
  STATE_MOVING_TO_1,
  STATE_MOVING_EXTRA,
  STATE_MOVING_TO_2
};
State state = STATE_HOMING;

uint8_t  ledBrightness   = 0;
uint8_t  ledTargetBright = 0;
uint32_t lastLedUpdate   = 0;
const uint8_t  FADE_STEP = 3;
const uint16_t FADE_MS   = 10;

bool     runningLight     = false;
uint16_t runnerPos        = 0;
uint32_t lastRunnerUpdate = 0;
const uint16_t RUNNER_MS  = 30;
const uint8_t  RUNNER_LEN = 30;

volatile char espnowCmd = 0;

uint8_t masterMAC[6]   = {0};
bool    masterMacKnown = false;

void onEspNowRecv(const uint8_t *mac,
                  const uint8_t *data, int len) {
  memcpy(masterMAC, mac, 6);
  masterMacKnown = true;
  if (len > 0) espnowCmd = (char)data[0];
}

// Learns the master's MAC from the first packet received, registers it as a
// peer, and sends short status strings back so the master can see in its own
// Serial log whether a command actually reached and was processed here.
void ackMaster(const char* msg) {
  if (!masterMacKnown) return;
  if (!esp_now_is_peer_exist(masterMAC)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, masterMAC, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }
  esp_now_send(masterMAC, (const uint8_t*)msg, strlen(msg));
}

bool endstopHit() { return digitalRead(ENDSTOP_PIN) == HIGH; }

void log(String msg) { Serial.println(msg); SerialBT.println(msg); }

// --- HELP ---
void printHelp() {
  String posStr;
  switch (currentPos) {
    case POS_1:       posStr = "POS_1 (убрана)";   break;
    case POS_2:       posStr = "POS_2 (выдвинута)"; break;
    default:          posStr = "UNKNOWN";            break;
  }
  String stateStr;
  switch (state) {
    case STATE_HOMING:       stateStr = "HOMING";        break;
    case STATE_IDLE:         stateStr = "IDLE";           break;
    case STATE_MOVING_TO_1:  stateStr = "MOVING → POS1"; break;
    case STATE_MOVING_EXTRA: stateStr = "MOVING (extra)"; break;
    case STATE_MOVING_TO_2:  stateStr = "MOVING → POS2"; break;
  }

  log("=============================");
  log("  ESP32 Stepper — справка");
  log("=============================");
  log("Команды:");
  log("  1      — переместить в POS 1 (убрать)");
  log("  2      — переместить в POS 2 (выдвинуть)");
  log("  H / ?  — эта справка");
  log("-----------------------------");
  log("Статус:");
  log("  Позиция : " + posStr);
  log("  Состояние: " + stateStr);
  log("  Endstop : " + String(endstopHit() ? "НАЖАТ" : "свободен"));
  log("  LED яркость: " + String(ledBrightness) +
      " → цель: " + String(ledTargetBright));
  log("  Бегущий огонь: " + String(runningLight ? "ВКЛ" : "ВЫКЛ"));
  log("-----------------------------");
  log("Конфигурация:");
  log("  STEPS_TO_POS2 : " + String(STEPS_TO_POS2));
  log("  EXTRA_STEPS   : " + String(EXTRA_STEPS));
  log("  NUM_LEDS      : " + String(NUM_LEDS));
  log("  FADE_STEP/MS  : " + String(FADE_STEP) + " / " + String(FADE_MS));
  log("  RUNNER_MS/LEN : " + String(RUNNER_MS) + " / " + String(RUNNER_LEN));
  log("-----------------------------");
  log("Сеть:");
  log("  MAC (ESP-NOW) : " + WiFi.macAddress());
  log("  BT имя        : ESP32_Stepper");
  log("-----------------------------");
  log("Пины:");
  log("  STEP=" + String(STEP_PIN) +
      "  DIR=" + String(DIR_PIN) +
      "  ENDSTOP=" + String(ENDSTOP_PIN) +
      "  LED=" + String(LED_PIN));
  log("=============================");
}

void setAllW(uint8_t w) {
  strip.fill(strip.Color(0, 0, 0, w));
  strip.show();
}

void updateRunner() {
  strip.clear();
  for (uint8_t j = 0; j < RUNNER_LEN; j++) {
    int idx = (int)runnerPos - j;
    if (idx < 0) idx += NUM_LEDS;
    uint8_t bright = (uint8_t)(255 * (RUNNER_LEN - j) / RUNNER_LEN);
    strip.setPixelColor(idx, strip.Color(0, 0, 0, bright));
  }
  strip.show();
  runnerPos = (runnerPos + 1) % NUM_LEDS;
}

void ledTick() {
  uint32_t now = millis();
  if (runningLight) {
    if (now - lastRunnerUpdate >= RUNNER_MS) {
      lastRunnerUpdate = now;
      updateRunner();
    }
    return;
  }
  if (now - lastLedUpdate >= FADE_MS) {
    lastLedUpdate = now;
    if (ledBrightness < ledTargetBright) {
      ledBrightness = (uint8_t)min((int)ledBrightness + FADE_STEP, (int)ledTargetBright);
      setAllW(ledBrightness);
    } else if (ledBrightness > ledTargetBright) {
      ledBrightness = (uint8_t)max((int)ledBrightness - FADE_STEP, (int)ledTargetBright);
      setAllW(ledBrightness);
    }
  }
}

void handleCmd(char cmd) {
  if (cmd == '1') {
    if (currentPos == POS_1)      { log("Already POS 1"); }
    else if (state != STATE_IDLE) { log("Busy"); }
    else {
      log("Moving to POS 1...");
      runningLight = false;
      ledTargetBright = 0;
      stepper.move(STEPS_TO_POS2);
      state = STATE_MOVING_TO_1;
    }
  } else if (cmd == '2') {
    if (currentPos == POS_2)      { log("Already POS 2"); }
    else if (state != STATE_IDLE) { log("Busy"); }
    else {
      log("Moving to POS 2...");
      ledTargetBright = 255;
      stepper.move(-STEPS_TO_POS2);
      state = STATE_MOVING_TO_2;
    }
  } else if (cmd == 'H' || cmd == 'h' || cmd == '?') {
    printHelp();
  } else {
    log("ERR: unknown cmd. Send H or ? for help");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ENDSTOP_PIN, INPUT_PULLUP);

  strip.begin();
  strip.setBrightness(255);
  strip.clear();
  strip.show();

  stepper.setMaxSpeed(5000);
  stepper.setAcceleration(1000);

  SerialBT.begin("ESP32_Stepper");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onEspNowRecv);
    log("ESP-NOW OK. MAC: " + WiFi.macAddress());
  } else {
    log("ESP-NOW FAIL");
  }

  log("Start. Homing... (send H for help)");
  stepper.move(100000L);
  state = STATE_HOMING;
}

void loop() {
  ledTick();

  if ((state == STATE_HOMING || state == STATE_MOVING_TO_1) && endstopHit()) {
    stepper.stop();
    stepper.setCurrentPosition(0);
    currentPos = POS_1;
    state = STATE_IDLE;
    runningLight = false;
    ledTargetBright = 0;
    log("POS 1 OK");
    return;
  }

  if (state == STATE_MOVING_TO_2 && !stepper.isRunning()) {
    currentPos = POS_2;
    state = STATE_IDLE;
    runningLight = true;
    runnerPos = 0;
    log("POS 2 OK");
  }

  if (state == STATE_MOVING_TO_1 && !stepper.isRunning()) {
    log("Endstop not hit, +100 steps...");
    stepper.move(EXTRA_STEPS);
    state = STATE_MOVING_EXTRA;
  }

  if (state == STATE_MOVING_EXTRA && !stepper.isRunning()) {
    if (endstopHit()) {
      stepper.setCurrentPosition(0);
      currentPos = POS_1;
      state = STATE_IDLE;
      runningLight = false;
      ledTargetBright = 0;
      log("POS 1 OK (extra)");
    } else {
      state = STATE_IDLE;
      runningLight = false;
      ledTargetBright = 0;
      log("ERR: endstop not triggered! Check pin 14");
    }
  }

  stepper.run();

  if (espnowCmd) {
    char cmd = espnowCmd;
    espnowCmd = 0;
    log("ESP-NOW cmd: " + String(cmd));
    handleCmd(cmd);
    ackMaster("CMD_OK");
  }

  if (SerialBT.available()) {
    char cmd = SerialBT.read();
    if (cmd == '\r' || cmd == '\n') return;
    handleCmd(cmd);
  }
}