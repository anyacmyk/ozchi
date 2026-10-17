#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

#define PULSE_STOP      1500
#define DEBOUNCE_MS     50UL
#define SERVO_PERIOD_US 20000UL
#define NUM_CHANNELS    6

const int PULSE_A[NUM_CHANNELS] = { 1100, 1100, 1100, 1100, 1100, 1900 };
const int PULSE_B[NUM_CHANNELS] = { 1900, 1900, 1900, 1900, 1900, 1100 };

const int SERVO_PINS[NUM_CHANNELS]  = { 17, 4, 8, 10, 12, 14 };
const int SWITCH_PINS[NUM_CHANNELS] = { 11, 9, 18, 15, 13, 5 };

enum State { HOMING, REVERSE, RETURN };

struct Channel {
  State         state;
  unsigned long reverseStart;
  unsigned long reverseDuration;
  int           lastDurationChoice;
  bool          lastRaw;
  bool          switchState;
  unsigned long lastDebounce;
  unsigned long lastPulseUs;
};

Channel ch[NUM_CHANNELS];

// =====================================================
// Shared protocol (MUST match master exactly)
// =====================================================
enum GamePhase : uint8_t {
  PHASE_ONBOARDING   = 0,
  PHASE_START        = 1,
  PHASE_FINISH       = 2,
  PHASE_COUNT_3      = 3,
  PHASE_COUNT_2      = 4,
  PHASE_COUNT_1      = 5,
  PHASE_ABORT        = 6,
  PHASE_RESET        = 7,
  PHASE_PANEL_STATUS = 8,
  PHASE_C_TICK       = 9
};

typedef struct {
  uint8_t phase;
  bool    success;
  bool    cheat;
  int     successCount;
  int     totalRounds;
  uint8_t panelState;
  int     missCount;
  int     roundCount;
} ResultMessage;

volatile bool gameActive  = false;
volatile bool gameStopped = false;

void sendPulse(int pin, int us) {
  digitalWrite(pin, HIGH);
  delayMicroseconds(us);
  digitalWrite(pin, LOW);
}

bool debounce(Channel &c, int pin) {
  bool raw = digitalRead(pin);
  if (raw != c.lastRaw) {
    c.lastDebounce = millis();
    c.lastRaw = raw;
  }
  if ((millis() - c.lastDebounce) >= DEBOUNCE_MS && raw != c.switchState) {
    c.switchState = raw;
    return true;
  }
  return false;
}

unsigned long nextReverseDuration(int i, Channel &c) {
  static const int POOL_MID[]   = { 0, 1, 2, 3, 4, 4, 4 };
  static const int POOL_OUTER[] = { 0, 1, 2, 3, 4, 4, 4, 4, 4 };
  static const int SIZE_MID   = sizeof(POOL_MID)   / sizeof(POOL_MID[0]);
  static const int SIZE_OUTER = sizeof(POOL_OUTER) / sizeof(POOL_OUTER[0]);

  bool  isOuter   = (i == 0 || i == NUM_CHANNELS - 1);
  int   poolSize  = isOuter ? SIZE_OUTER : SIZE_MID;
  const int *pool = isOuter ? POOL_OUTER : POOL_MID;

  int randomChoice;
  do {
    randomChoice = pool[random(0, poolSize)];
  } while (randomChoice == c.lastDurationChoice);

  c.lastDurationChoice = randomChoice;

  const unsigned long DURATIONS[] = {
    5000UL, 10000UL, 15000UL, 20000UL, 26000UL
  };

  return DURATIONS[randomChoice];
}

void streamStopPulses() {
  static unsigned long lastStopUs = 0;
  unsigned long nowUs = micros();
  if (nowUs - lastStopUs >= SERVO_PERIOD_US) {
    for (int i = 0; i < NUM_CHANNELS; i++) {
      sendPulse(SERVO_PINS[i], PULSE_STOP);
    }
    lastStopUs = nowUs;
  }
}

void resetChannelsToIdle() {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    ch[i].state = HOMING;
    ch[i].reverseStart = 0;
    ch[i].reverseDuration = 0;
    ch[i].lastDurationChoice = -1;
    ch[i].lastRaw = HIGH;
    ch[i].switchState = HIGH;
    ch[i].lastDebounce = 0;
    ch[i].lastPulseUs = 0;
  }
  gameActive = false;
  gameStopped = false;
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;

  if (len != (int)sizeof(ResultMessage)) {
    Serial.printf("[ESP-NOW] bad length %d (expected %u)\n",
                  len, (unsigned)sizeof(ResultMessage));
    return;
  }

  ResultMessage msg;
  memcpy(&msg, data, sizeof(msg));

  if (msg.phase == PHASE_RESET) {
    resetChannelsToIdle();
    Serial.println("[ESP-NOW] PHASE_RESET -- servo board idle again");
  }
  else if (msg.phase == PHASE_START) {
    if (!gameStopped) {
      gameActive = true;
      Serial.println("[ESP-NOW] PHASE_START -- servos enabled");
    }
  }
  else if (msg.phase == PHASE_FINISH || msg.phase == PHASE_ABORT) {
    gameActive  = false;
    gameStopped = true;
    Serial.println("[ESP-NOW] STOP -- servos halted immediately");
  }
  // PHASE_PANEL_STATUS are ignored by B
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  Serial.print("Servo ESP32 MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);
  Serial.println("ESP-NOW ready (listening).");
}

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());
  delay(500);

  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(SERVO_PINS[i], OUTPUT);
    pinMode(SWITCH_PINS[i], INPUT_PULLUP);
    digitalWrite(SERVO_PINS[i], LOW);
    ch[i] = { HOMING, 0, 0, -1, HIGH, HIGH, 0, 0 };
  }

  setupEspNow();

  unsigned long t = millis();
  while (millis() - t < 1000) {
    for (int i = 0; i < NUM_CHANNELS; i++) {
      sendPulse(SERVO_PINS[i], PULSE_STOP);
    }
    delay(20);
  }

  Serial.println("Servo controller idle -- waiting for PHASE_START...");
}

void loop() {
  if (gameStopped) {
    streamStopPulses();
    return;
  }

  if (!gameActive) {
    streamStopPulses();
    return;
  }

  for (int i = 0; i < NUM_CHANNELS; i++) {
    Channel &c = ch[i];

    bool changed = debounce(c, SWITCH_PINS[i]);
    bool pressed = (c.switchState == LOW);

    unsigned long now = millis();
    int pulse = PULSE_STOP;

    switch (c.state) {

      case HOMING:
        pulse = PULSE_A[i];
        if (pressed && changed) {
          c.state = REVERSE;
          c.reverseStart = now;
          c.reverseDuration = nextReverseDuration(i, c);
          Serial.printf("Ch%d -> REVERSE (%lu ms)\n", i, c.reverseDuration);
        }
        break;

      case REVERSE:
        pulse = PULSE_B[i];
        if (now - c.reverseStart >= c.reverseDuration) {
          c.state = RETURN;
          Serial.printf("Ch%d -> RETURN\n", i);
        }
        break;

      case RETURN:
        pulse = PULSE_A[i];
        if (pressed && changed) {
          c.state = REVERSE;
          c.reverseStart = now;
          c.reverseDuration = nextReverseDuration(i, c);
          Serial.printf("Ch%d -> REVERSE again (%lu ms)\n", i, c.reverseDuration);
        }
        break;
    }

    unsigned long nowUs = micros();
    if ((nowUs - c.lastPulseUs) >= SERVO_PERIOD_US) {
      sendPulse(SERVO_PINS[i], pulse);
      c.lastPulseUs = nowUs;
    }
  }
}
