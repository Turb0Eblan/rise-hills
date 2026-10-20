#include <Arduino.h>
#include <AccelStepper.h>
#include <BluetoothSerial.h>
#include <Preferences.h>
#include <esp_now.h>
#include <WiFi.h>

BluetoothSerial SerialBT;
Preferences     prefs;

// =======================
// Пины STEP / DIR
// =======================
#define M1_STEP 25
#define M1_DIR  26
#define M3_STEP 12
#define M3_DIR  13
#define M2_STEP 27
#define M2_DIR  14

// =======================
// Концевики (HIGH = сработал, INPUT_PULLUP)
// =======================
#define LIMIT_M1 36
#define LIMIT_M3 34
#define LIMIT_M2 39

// =======================
// Настройки по умолчанию
// =======================
#define DEFAULT_STEPS_M1    9000L
#define DEFAULT_STEPS_M2    60000L
#define DEFAULT_STEPS_M3    35000L
#define DEFAULT_MAX_SPEED   3000.0f
#define DEFAULT_ACCEL       800.0f
#define DEFAULT_DELAY1_MS   5000UL   // пауза после старта M1 (при опускании) / M2 (при подъёме)
#define DEFAULT_DELAY2_MS   5000UL   // пауза после старта M3
#define DEFAULT_DELAY3_MS   5000UL   // пауза после старта M2 (при опускании) / M1 (при подъёме)

const long EXTRA_STEPS = 100;

// =======================
// Рабочие параметры (загружаются из NVS)
// =======================
long          moveStepsM1  = DEFAULT_STEPS_M1;
long          moveStepsM2  = DEFAULT_STEPS_M2;
long          moveStepsM3  = DEFAULT_STEPS_M3;
float         maxSpeed     = DEFAULT_MAX_SPEED;
float         acceleration = DEFAULT_ACCEL;
unsigned long delayMs1     = DEFAULT_DELAY1_MS;  // после старта M1↓ / M2↑
unsigned long delayMs2     = DEFAULT_DELAY2_MS;  // после старта M3
unsigned long delayMs3     = DEFAULT_DELAY3_MS;  // после старта M2↓ / M1↑

// =======================
// ESP-NOW
// =======================
uint8_t masterMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool    masterMacKnown = false;

typedef struct {
  uint8_t cmd;
  uint8_t reserved;
} EspNowCmd;

volatile bool    espNowCmdPending = false;
volatile uint8_t espNowCmdVal     = 0;

// Learns the master's MAC from the first packet it receives, registers it
// as an ESP-NOW peer, and sends short status strings back so the master can
// confirm the slave actually received/processed a command (see Master.cpp
// onDataRecv). Without this the master has no way to tell a dropped packet
// apart from a silently-ignored one.
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

// =======================
// Драйверы
// =======================
AccelStepper motor1(AccelStepper::DRIVER, M1_STEP, M1_DIR);
AccelStepper motor2(AccelStepper::DRIVER, M2_STEP, M2_DIR);
AccelStepper motor3(AccelStepper::DRIVER, M3_STEP, M3_DIR);

#define DOWN_DIR  1
#define UP_DIR   -1

// =======================
// Состояния
// =======================
enum SystemState  { STATE_INIT, STATE_IDLE, STATE_GOING_DOWN, STATE_GOING_UP, STATE_ERROR };
enum Position     { POS_UNKNOWN = 0, POS_DOWN = 1, POS_UP = 2 };
enum MoveSubState { SUB_START_1, SUB_WAIT_1, SUB_START_2, SUB_WAIT_2, SUB_START_3, SUB_WAIT_3, SUB_DONE };

SystemState  sysState    = STATE_INIT;
Position     sysPosition = POS_UNKNOWN;
MoveSubState subState     = SUB_START_1;
MoveSubState initSubState = SUB_START_1;

unsigned long subTimer = 0;
bool m1Done = false, m2Done = false, m3Done = false;
bool m1Extra = false, m2Extra = false, m3Extra = false;

// =======================
// NVS
// =======================
void loadSettings() {
  prefs.begin("lift", true);
  moveStepsM1  = prefs.getLong  ("stepsM1",  DEFAULT_STEPS_M1);
  moveStepsM2  = prefs.getLong  ("stepsM2",  DEFAULT_STEPS_M2);
  moveStepsM3  = prefs.getLong  ("stepsM3",  DEFAULT_STEPS_M3);
  maxSpeed     = prefs.getFloat ("maxSpeed", DEFAULT_MAX_SPEED);
  acceleration = prefs.getFloat ("accel",    DEFAULT_ACCEL);
  delayMs1     = prefs.getULong ("dly1",     DEFAULT_DELAY1_MS);
  delayMs2     = prefs.getULong ("dly2",     DEFAULT_DELAY2_MS);
  delayMs3     = prefs.getULong ("dly3",     DEFAULT_DELAY3_MS);
  prefs.end();
  Serial.printf("NVS loaded: M1=%ld M2=%ld M3=%ld spd=%.0f acc=%.0f dly1=%lu dly2=%lu dly3=%lu\n",
                moveStepsM1, moveStepsM2, moveStepsM3, maxSpeed, acceleration, delayMs1, delayMs2, delayMs3);
}

void saveSettings() {
  prefs.begin("lift", false);
  prefs.putLong  ("stepsM1",  moveStepsM1);
  prefs.putLong  ("stepsM2",  moveStepsM2);
  prefs.putLong  ("stepsM3",  moveStepsM3);
  prefs.putFloat ("maxSpeed", maxSpeed);
  prefs.putFloat ("accel",    acceleration);
  prefs.putULong ("dly1",     delayMs1);
  prefs.putULong ("dly2",     delayMs2);
  prefs.putULong ("dly3",     delayMs3);
  prefs.end();
  Serial.println("NVS saved.");
  SerialBT.println("NVS saved.");
}

void applyMotorSettings() {
  motor1.setMaxSpeed(maxSpeed);  motor1.setAcceleration(acceleration);
  motor2.setMaxSpeed(maxSpeed);  motor2.setAcceleration(acceleration);
  motor3.setMaxSpeed(maxSpeed);  motor3.setAcceleration(acceleration);
}

void resetSettings() {
  moveStepsM1  = DEFAULT_STEPS_M1;
  moveStepsM2  = DEFAULT_STEPS_M2;
  moveStepsM3  = DEFAULT_STEPS_M3;
  maxSpeed     = DEFAULT_MAX_SPEED;
  acceleration = DEFAULT_ACCEL;
  delayMs1     = DEFAULT_DELAY1_MS;
  delayMs2     = DEFAULT_DELAY2_MS;
  delayMs3     = DEFAULT_DELAY3_MS;
  saveSettings();
  applyMotorSettings();
  Serial.println("Settings reset to defaults.");
  SerialBT.println("Settings reset to defaults.");
}

void printHelp() {
  SerialBT.println("========= LIFT CONTROLLER HELP =========");
  SerialBT.println("--- Control ---");
  SerialBT.println("  1        : go DOWN");
  SerialBT.println("  2        : go UP");
  SerialBT.println("  0        : EMERGENCY STOP");
  SerialBT.println("--- Motor steps ---");
  SerialBT.println("  M1:<N>   : set steps for motor 1 (100..500000)");
  SerialBT.println("  M2:<N>   : set steps for motor 2");
  SerialBT.println("  M3:<N>   : set steps for motor 3");
  SerialBT.println("--- Motion params ---");
  SerialBT.println("  SPD:<N>  : max speed, steps/s (100..10000)");
  SerialBT.println("  ACC:<N>  : acceleration, steps/s^2 (50..5000)");
  SerialBT.println("  DLY1:<N> : delay after M1 start (DOWN) / M2 start (UP), ms (0..30000)");
  SerialBT.println("  DLY2:<N> : delay after M3 start, ms (0..30000)");
  SerialBT.println("  DLY3:<N> : delay after M2 start (DOWN) / M1 start (UP), ms (0..30000)");
  SerialBT.println("--- System ---");
  SerialBT.println("  i / Help : this help + current settings + MAC");
  SerialBT.println("  RESET    : restore factory defaults");
  SerialBT.println("=========================================");

  // Current settings block
  SerialBT.println("--- Current settings ---");
  SerialBT.printf ("  M1 steps  : %ld\n",  moveStepsM1);
  SerialBT.printf ("  M2 steps  : %ld\n",  moveStepsM2);
  SerialBT.printf ("  M3 steps  : %ld\n",  moveStepsM3);
  SerialBT.printf ("  Max speed : %.0f steps/s\n", maxSpeed);
  SerialBT.printf ("  Accel     : %.0f steps/s^2\n", acceleration);
  SerialBT.printf ("  Delay 1   : %lu ms (after M1 start DOWN / M2 start UP)\n", delayMs1);
  SerialBT.printf ("  Delay 2   : %lu ms (after M3 start)\n", delayMs2);
  SerialBT.printf ("  Delay 3   : %lu ms (after M2 start DOWN / M1 start UP)\n", delayMs3);
  SerialBT.printf ("  Position  : %s\n",
    sysPosition == POS_DOWN ? "DOWN" :
    sysPosition == POS_UP   ? "UP"   : "UNKNOWN");
  SerialBT.printf ("  State     : %s\n",
    sysState == STATE_IDLE       ? "IDLE"       :
    sysState == STATE_GOING_DOWN ? "GOING_DOWN" :
    sysState == STATE_GOING_UP   ? "GOING_UP"   :
    sysState == STATE_INIT       ? "INIT"        : "ERROR");
  SerialBT.printf ("  MAC       : %s\n", WiFi.macAddress().c_str());
  SerialBT.println("========================================");
}

// =======================
// ESP-NOW
// =======================
void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  memcpy(masterMAC, mac, 6);
  masterMacKnown = true;
  if (len < (int)sizeof(EspNowCmd)) return;
  EspNowCmd pkt;
  memcpy(&pkt, data, sizeof(pkt));
  espNowCmdVal     = pkt.cmd;
  espNowCmdPending = true;
}

void initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init FAILED");
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  Serial.printf("ESP-NOW ready. MAC: %s\n", WiFi.macAddress().c_str());
  SerialBT.printf("ESP-NOW ready. MAC: %s\n", WiFi.macAddress().c_str());
}

// =======================
// Утилиты
// =======================
bool limitM1() { return digitalRead(LIMIT_M1) == HIGH; }
bool limitM2() { return digitalRead(LIMIT_M2) == HIGH; }
bool limitM3() { return digitalRead(LIMIT_M3) == HIGH; }

void hardStopMotor(AccelStepper &m) { m.setCurrentPosition(m.currentPosition()); }

void startMotor(AccelStepper &m, long steps, int dir) { m.move(steps * dir); }

// =======================
// ИНИЦИАЛИЗАЦИЯ (хоминг): M1 -> M3 -> M2
// =======================
void runInit() {
  switch (initSubState) {
    case SUB_START_1:
      if (!limitM1()) startMotor(motor1, moveStepsM1 + EXTRA_STEPS * 10, DOWN_DIR);
      subTimer = millis(); initSubState = SUB_WAIT_1;
      Serial.println("INIT: M1 started"); SerialBT.println("INIT: M1 started");
      break;
    case SUB_WAIT_1:
      if (limitM1() && !m1Done) { hardStopMotor(motor1); m1Done = true;
        Serial.println("INIT: M1 limit"); SerialBT.println("INIT: M1 limit"); }
      if (millis() - subTimer >= delayMs1) initSubState = SUB_START_2;
      break;
    case SUB_START_2:
      if (!limitM3()) startMotor(motor3, moveStepsM3 + EXTRA_STEPS * 10, DOWN_DIR);
      subTimer = millis(); initSubState = SUB_WAIT_2;
      Serial.println("INIT: M3 started"); SerialBT.println("INIT: M3 started");
      break;
    case SUB_WAIT_2:
      if (limitM1() && !m1Done) { hardStopMotor(motor1); m1Done = true; }
      if (limitM3() && !m3Done) { hardStopMotor(motor3); m3Done = true;
        Serial.println("INIT: M3 limit"); SerialBT.println("INIT: M3 limit"); }
      if (millis() - subTimer >= delayMs2) initSubState = SUB_START_3;
      break;
    case SUB_START_3:
      if (!limitM2()) startMotor(motor2, moveStepsM2 + EXTRA_STEPS * 10, DOWN_DIR);
      initSubState = SUB_WAIT_3;
      Serial.println("INIT: M2 started"); SerialBT.println("INIT: M2 started");
      break;
    case SUB_WAIT_3:
      if (limitM1() && !m1Done) { hardStopMotor(motor1); m1Done = true; }
      if (limitM3() && !m3Done) { hardStopMotor(motor3); m3Done = true; }
      if (limitM2() && !m2Done) { hardStopMotor(motor2); m2Done = true;
        Serial.println("INIT: M2 limit"); SerialBT.println("INIT: M2 limit"); }
      if (m1Done && m2Done && m3Done) {
        Serial.println("INIT_DONE"); SerialBT.println("INIT_DONE");
        sysPosition = POS_DOWN; sysState = STATE_IDLE;
        initSubState = SUB_START_1; m1Done = m2Done = m3Done = false;
      }
      break;
    default: break;
  }
}

// =======================
// ОПУСКАНИЕ: M1 -> M3 -> M2
// =======================
void startGoDown() {
  subState = SUB_START_1;
  m1Done = m2Done = m3Done = false;
  m1Extra = m2Extra = m3Extra = false;
  sysState = STATE_GOING_DOWN;
  Serial.println("CMD: DOWN"); SerialBT.println("CMD: DOWN");
}

void runGoDown() {
  auto checkMotor = [](AccelStepper &m, bool &done, bool &extra,
                       bool (*limit)(), const char *name) {
    if (done) return;
    if (limit()) {
      hardStopMotor(m); done = true;
      Serial.printf("%s: limit\n", name); SerialBT.printf("%s: limit\n", name);
    } else if (m.distanceToGo() == 0 && !extra) {
      startMotor(m, EXTRA_STEPS, DOWN_DIR); extra = true;
    } else if (m.distanceToGo() == 0 && extra) {
      done = true;
      Serial.printf("ERROR: %s limit not triggered!\n", name);
      SerialBT.printf("ERROR: %s limit not triggered!\n", name);
      sysState = STATE_ERROR;
    }
  };

  switch (subState) {
    case SUB_START_1:
      startMotor(motor1, moveStepsM1, DOWN_DIR);
      subTimer = millis(); subState = SUB_WAIT_1;
      Serial.println("DOWN: M1"); SerialBT.println("DOWN: M1");
      break;
    case SUB_WAIT_1:
      checkMotor(motor1, m1Done, m1Extra, limitM1, "M1");
      if (sysState == STATE_ERROR) return;
      if (millis() - subTimer >= delayMs1) subState = SUB_START_2;
      break;
    case SUB_START_2:
      startMotor(motor3, moveStepsM3, DOWN_DIR);
      subTimer = millis(); subState = SUB_WAIT_2;
      Serial.println("DOWN: M3"); SerialBT.println("DOWN: M3");
      break;
    case SUB_WAIT_2:
      checkMotor(motor1, m1Done, m1Extra, limitM1, "M1");
      checkMotor(motor3, m3Done, m3Extra, limitM3, "M3");
      if (sysState == STATE_ERROR) return;
      if (millis() - subTimer >= delayMs2) subState = SUB_START_3;
      break;
    case SUB_START_3:
      startMotor(motor2, moveStepsM2, DOWN_DIR);
      subState = SUB_WAIT_3;
      Serial.println("DOWN: M2"); SerialBT.println("DOWN: M2");
      break;
    case SUB_WAIT_3:
      checkMotor(motor1, m1Done, m1Extra, limitM1, "M1");
      checkMotor(motor3, m3Done, m3Extra, limitM3, "M3");
      checkMotor(motor2, m2Done, m2Extra, limitM2, "M2");
      if (sysState == STATE_ERROR) return;
      if (m1Done && m2Done && m3Done) {
        sysPosition = POS_DOWN; sysState = STATE_IDLE; subState = SUB_DONE;
        Serial.println("DOWN_DONE"); SerialBT.println("DOWN_DONE");
        ackMaster("DOWN_DONE");
      }
      break;
    default: break;
  }
}

// =======================
// ПОДЪЁМ: M2 -> M3 -> M1
// =======================
void startGoUp() {
  subState = SUB_START_1;
  m1Done = m2Done = m3Done = false;
  sysState = STATE_GOING_UP;
  subTimer = millis();
  Serial.println("CMD: UP"); SerialBT.println("CMD: UP");
}

void runGoUp() {
  switch (subState) {
    case SUB_START_1:
      startMotor(motor2, moveStepsM2, UP_DIR);
      subTimer = millis(); subState = SUB_WAIT_1;
      Serial.println("UP: M2"); SerialBT.println("UP: M2");
      break;
    case SUB_WAIT_1:
      if (millis() - subTimer >= delayMs3) subState = SUB_START_2;  // обратный порядок: dly3
      break;
    case SUB_START_2:
      startMotor(motor3, moveStepsM3, UP_DIR);
      subTimer = millis(); subState = SUB_WAIT_2;
      Serial.println("UP: M3"); SerialBT.println("UP: M3");
      break;
    case SUB_WAIT_2:
      if (millis() - subTimer >= delayMs2) subState = SUB_START_3;  // dly2
      break;
    case SUB_START_3:
      startMotor(motor1, moveStepsM1, UP_DIR);
      subState = SUB_WAIT_3;
      Serial.println("UP: M1"); SerialBT.println("UP: M1");
      break;
    case SUB_WAIT_3:
      if (motor1.distanceToGo() == 0 && motor2.distanceToGo() == 0 && motor3.distanceToGo() == 0) {
        sysPosition = POS_UP; sysState = STATE_IDLE; subState = SUB_DONE;
        Serial.println("UP_DONE"); SerialBT.println("UP_DONE");
        ackMaster("UP_DONE");
      }
      break;
    default: break;
  }
}

// =======================
// Bluetooth-команды
// =======================
// Управление:
//   1            — опустить
//   2            — поднять
//   0            — аварийная остановка
//
// Шаги двигателей:
//   M1:<N>       — шаги M1  (100..500000)
//   M2:<N>       — шаги M2
//   M3:<N>       — шаги M3
//
// Параметры движения:
//   SPD:<N>      — макс. скорость, шагов/с  (100..10000)
//   ACC:<N>      — ускорение, шагов/с²      (50..5000)
//   DLY1:<N>     — задержка после старта M1↓ / M2↑, мс (0..30000)
//   DLY2:<N>     — задержка после старта M3, мс (0..30000)
//   DLY3:<N>     — задержка после старта M2↓ / M1↑, мс (0..30000)
//
// Системные:
//   i  / Help    — справка + текущие настройки + MAC
//   RESET        — сброс к заводским значениям
// =======================

String btBuffer = "";

void handleSerial() {
  while (SerialBT.available()) {
    char c = SerialBT.read();
    if (c == '\n' || c == '\r') {
      btBuffer.trim();
      if (btBuffer.length() == 0) { btBuffer = ""; return; }
      String line = btBuffer;
      btBuffer = "";

      // ---- Справка ----
      if (line.equalsIgnoreCase("i") || line.equalsIgnoreCase("help")) {
        printHelp();
        return;
      }

      // ---- Сброс ----
      if (line.equalsIgnoreCase("RESET")) {
        if (sysState != STATE_IDLE) { SerialBT.println("BUSY"); return; }
        resetSettings();
        return;
      }

      // ---- Вспомогательная функция: парсинг long-параметра после ':' ----
      auto parseParam = [&](const String &prefix, long minV, long maxV, long &target, const char *label) -> bool {
        if (!line.startsWith(prefix)) return false;
        if (sysState != STATE_IDLE) { SerialBT.println("BUSY"); return true; }
        long val = line.substring(prefix.length()).toInt();
        if (val < minV || val > maxV) {
          SerialBT.printf("ERROR: %s out of range (%ld..%ld)\n", label, minV, maxV);
          return true;
        }
        target = val;
        saveSettings();
        SerialBT.printf("OK: %s = %ld\n", label, target);
        return true;
      };

      // ---- Шаги двигателей ----
      { long tmp; if (parseParam("M1:", 100, 500000, tmp=moveStepsM1, "M1 steps")) { if(tmp!=moveStepsM1){moveStepsM1=tmp;} return; } }
      { long tmp; if (parseParam("M2:", 100, 500000, tmp=moveStepsM2, "M2 steps")) { if(tmp!=moveStepsM2){moveStepsM2=tmp;} return; } }
      { long tmp; if (parseParam("M3:", 100, 500000, tmp=moveStepsM3, "M3 steps")) { if(tmp!=moveStepsM3){moveStepsM3=tmp;} return; } }

      // ---- Скорость ----
      if (line.startsWith("SPD:") || line.startsWith("spd:")) {
        if (sysState != STATE_IDLE) { SerialBT.println("BUSY"); return; }
        long val = line.substring(4).toInt();
        if (val < 100 || val > 10000) { SerialBT.println("ERROR: SPD out of range (100..10000)"); return; }
        maxSpeed = (float)val;
        applyMotorSettings();
        saveSettings();
        SerialBT.printf("OK: max speed = %.0f steps/s\n", maxSpeed);
        return;
      }

      // ---- Ускорение ----
      if (line.startsWith("ACC:") || line.startsWith("acc:")) {
        if (sysState != STATE_IDLE) { SerialBT.println("BUSY"); return; }
        long val = line.substring(4).toInt();
        if (val < 50 || val > 5000) { SerialBT.println("ERROR: ACC out of range (50..5000)"); return; }
        acceleration = (float)val;
        applyMotorSettings();
        saveSettings();
        SerialBT.printf("OK: acceleration = %.0f steps/s^2\n", acceleration);
        return;
      }

      // ---- Задержки ----
      auto parseDly = [&](const char *prefix, unsigned long &target, const char *label) -> bool {
        if (!line.startsWith(prefix)) return false;
        if (sysState != STATE_IDLE) { SerialBT.println("BUSY"); return true; }
        long val = line.substring(strlen(prefix)).toInt();
        if (val < 0 || val > 30000) {
          SerialBT.printf("ERROR: %s out of range (0..30000)\n", label);
          return true;
        }
        target = (unsigned long)val;
        saveSettings();
        SerialBT.printf("OK: %s = %lu ms\n", label, target);
        return true;
      };
      if (parseDly("DLY1:", delayMs1, "DLY1") || parseDly("dly1:", delayMs1, "DLY1")) return;
      if (parseDly("DLY2:", delayMs2, "DLY2") || parseDly("dly2:", delayMs2, "DLY2")) return;
      if (parseDly("DLY3:", delayMs3, "DLY3") || parseDly("dly3:", delayMs3, "DLY3")) return;

      // ---- Управляющие команды ----
      if (line == "0") {
        hardStopMotor(motor1); hardStopMotor(motor2); hardStopMotor(motor3);
        sysState = STATE_IDLE; sysPosition = POS_UNKNOWN;
        Serial.println("EMERGENCY_STOP"); SerialBT.println("EMERGENCY_STOP");
        return;
      }
      if (line == "1") {
        if (sysState == STATE_INIT || sysState == STATE_GOING_DOWN || sysState == STATE_GOING_UP)
          SerialBT.println("BUSY");
        else if (sysState == STATE_ERROR)
          SerialBT.println("ERROR_STATE: send 0 to reset");
        else if (sysPosition == POS_DOWN)
          SerialBT.println("ALREADY_DOWN");
        else
          startGoDown();
        return;
      }
      if (line == "2") {
        if (sysState == STATE_INIT || sysState == STATE_GOING_DOWN || sysState == STATE_GOING_UP)
          SerialBT.println("BUSY");
        else if (sysState == STATE_ERROR)
          SerialBT.println("ERROR_STATE: send 0 to reset");
        else if (sysPosition == POS_UP)
          SerialBT.println("ALREADY_UP");
        else
          startGoUp();
        return;
      }

      SerialBT.printf("UNKNOWN CMD: %s\n", line.c_str());
    } else {
      btBuffer += c;
      if (btBuffer.length() > 32) btBuffer = "";
    }
  }
}

// =======================
// ESP-NOW команды
// =======================
void handleEspNow() {
  if (!espNowCmdPending) return;
  espNowCmdPending = false;
  uint8_t cmd = espNowCmdVal;
  Serial.printf("ESP-NOW cmd: %d\n", cmd);
  if (cmd == 0) {
    hardStopMotor(motor1); hardStopMotor(motor2); hardStopMotor(motor3);
    sysState = STATE_IDLE; sysPosition = POS_UNKNOWN;
    Serial.println("ESP-NOW: STOP"); SerialBT.println("ESP-NOW: STOP");
    ackMaster("STOP_OK");
    return;
  }
  if (cmd == 1) {
    if (sysState != STATE_IDLE) { SerialBT.println("ESP-NOW: BUSY"); ackMaster("BUSY"); return; }
    if (sysState == STATE_ERROR) { SerialBT.println("ESP-NOW: ERROR_STATE"); ackMaster("ERROR_STATE"); return; }
    if (sysPosition == POS_DOWN) { SerialBT.println("ESP-NOW: ALREADY_DOWN"); ackMaster("ALREADY_DOWN"); return; }
    ackMaster("DOWN_STARTED");
    startGoDown(); return;
  }
  if (cmd == 2) {
    if (sysState != STATE_IDLE) { SerialBT.println("ESP-NOW: BUSY"); ackMaster("BUSY"); return; }
    if (sysState == STATE_ERROR) { SerialBT.println("ESP-NOW: ERROR_STATE"); ackMaster("ERROR_STATE"); return; }
    if (sysPosition == POS_UP)  { SerialBT.println("ESP-NOW: ALREADY_UP"); ackMaster("ALREADY_UP"); return; }
    ackMaster("UP_STARTED");
    startGoUp(); return;
  }
  Serial.printf("ESP-NOW: unknown cmd %d\n", cmd);
  ackMaster("UNKNOWN_CMD");
}

// =======================
// SETUP / LOOP
// =======================
void setup() {
  Serial.begin(115200);
  SerialBT.begin("Building Layout");

  pinMode(LIMIT_M1, INPUT_PULLUP);
  pinMode(LIMIT_M2, INPUT_PULLUP);
  pinMode(LIMIT_M3, INPUT_PULLUP);

  loadSettings();
  applyMotorSettings();
  initEspNow();

  sysState = STATE_INIT; initSubState = SUB_START_1;
  m1Done = m2Done = m3Done = false;
  Serial.println("BOOT: homing...");
  SerialBT.println("BOOT: homing... (send 'i' for help)");
}

void loop() {
  motor1.run();
  motor2.run();
  motor3.run();

  handleSerial();
  handleEspNow();

  switch (sysState) {
    case STATE_INIT:       runInit();   break;
    case STATE_GOING_DOWN: runGoDown(); break;
    case STATE_GOING_UP:   runGoUp();   break;
    case STATE_IDLE:                    break;
    case STATE_ERROR:
      static unsigned long errTimer = 0;
      if (millis() - errTimer > 3000) {
        Serial.println("ERROR: halted. Send 0 to reset.");
        SerialBT.println("ERROR: halted. Send 0 to reset.");
        errTimer = millis();
      }
      break;
  }
}
