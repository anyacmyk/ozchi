#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// =====================================================
// MASTER = WEB CONTROL + PANEL LED/PIEZO + AUDIO BRIDGE
// Phone connects to this Wi-Fi and opens 192.168.4.1
// D sends START / RESET / ABORT to panel/servo via ESP-NOW.
// Panel NeoPixel strips remain controlled by the integrated panel code.
// Strike logic: HAND or FOOT buttons on the phone.
// Each press is forwarded to Python over Serial so Python plays voice.
// 3 strikes (any combination) -> automatic ABORT -> CHEAT ending.
// =====================================================

enum LedMode : uint8_t {
  LED_OFF          = 0,
  LED_CALIBRATION  = 1,
  LED_WAIT_HIT     = 2,
  LED_HIT          = 3,
  LED_MISS         = 4,
  LED_END_SUCCESS  = 5,
  LED_END_FAIL     = 6,
  LED_END_CHEAT    = 7,
  LED_CHASE        = 8,
  LED_COUNTDOWN    = 9    // white pulse during 3-2-1 countdown
};

volatile LedMode  ledMode      = LED_OFF;
unsigned long     ledStateTs   = 0;
int               chaseStep    = 0;
bool              chaseFlashOn = false;
int               chaseFlashCount = 0;

// ============================================
// WIFI
// ============================================
const char* WIFI_NAME = "Elmo'sCircus_GameControl";
const char* WIFI_PASS = "12345678";

WebServer server(80);

// ============================================
// Board MAC addresses
// ============================================
uint8_t bMac[] = { 0xD0, 0xCF, 0x13, 0x52, 0x2D, 0x28 };

// ============================================
// Shared protocol (MUST match panel/servo exactly)
// ============================================
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
  PHASE_C_TICK       = 9,
  PHASE_RESULT_DONE       = 10
};

enum PanelState : uint8_t {
  PSTATE_CLEAR       = 0,
  PSTATE_WAIT        = 1,
  PSTATE_HIT         = 2,
  PSTATE_MISS        = 3,
  PSTATE_CALIBRATION = 4
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

ResultMessage outMsg;

// ============================================
// AUTO RESET CONFIG
// When D board boots/resets, it sends PHASE_RESET to panel/servo.
// A board now uses software reset and auto-starts by itself,
// so D does NOT need to wait and send START after reset.
// ============================================


// ============================================
// Game state mirror (for web page)
// ============================================
String  lastStatus    = "Ready";
int     uiHits        = 0;
int     uiMisses      = 0;
int     uiRound       = 0;
int     uiTotalRounds = 20;
int     strikeCount   = 0;
String  lastStrike    = "";
bool    gameOver      = false;
bool    gameRunning   = false;
String  endResult     = "";
bool    calibratingSent = false;   // true after "CALIBRATING" sent to Python this game

// Final LED sequence control:
// D first flashes white while result animation move.
// When result animation completes, D switches to the final result colours.
bool          waitingForCFinalLed = false;
LedMode       pendingFinalLedMode = LED_OFF;
unsigned long waitingForCStartMs  = 0;
const unsigned long C_FINAL_FALLBACK_MS = 5000;  // fallback if RESULT_DONE is missed

// ============================================
// ONBOARDING duplicate suppression
// A board may send PHASE_ONBOARDING more than once during its boot/
// software-reset sequence. We only want to emit "ONBOARDING" on
// Serial once per game, so Python plays the voice line once.
// `lastOnboardingSentMs == 0` means "no ONBOARDING sent yet, next one
// is allowed". sendResetAll() clears it to 0 so the next game can talk.
// ============================================
unsigned long       lastOnboardingSentMs = 0;
const unsigned long ONBOARDING_DEBOUNCE_MS = 30000;  // belt-and-braces window

// ============================================
// SIMULATE mode
// ============================================
enum SimStep : uint8_t {
  SIM_OFF        = 0,
  SIM_ONBOARDING = 1,
  SIM_CAL_WHITE  = 2,
  SIM_GO         = 3,
  SIM_ROUND_WAIT = 4,
  SIM_ROUND_HIT  = 5,
  SIM_DONE       = 6
};

bool          simActive     = false;
SimStep       simStep       = SIM_OFF;
unsigned long simStepTs     = 0;
int           simSubStep    = 0;
int           simRound      = 0;
int           simHits       = 0;
int           simMisses     = 0;
const int     SIM_ROUNDS    = 8;

// Forward declarations used by resetAllThenAutoStart()


void panel_setup();
void panel_loop();
struct panel_ResultMessage;
void dispatchPanelToMaster(const panel_ResultMessage &msg);
void requestPanelPhase(GamePhase phase);

// ============================================
// ESP-NOW send helpers
// ============================================
void sendToPeer(const uint8_t *mac, const char *name, GamePhase phase,
                bool success = false, bool cheat = false,
                int successCount = 0, int totalRounds = 20) {
  outMsg.phase        = (uint8_t)phase;
  outMsg.success      = success;
  outMsg.cheat        = cheat;
  outMsg.successCount = successCount;
  outMsg.totalRounds  = totalRounds;
  outMsg.panelState   = PSTATE_CLEAR;
  outMsg.missCount    = 0;
  outMsg.roundCount   = 0;

  esp_err_t result = esp_now_send(mac, (uint8_t *)&outMsg, sizeof(outMsg));

  Serial.print("Send to ");
  Serial.print(name);
  Serial.print(" phase=");
  Serial.print(phase);
  Serial.print(" -> ");
  Serial.println(result == ESP_OK ? "OK" : "FAILED");
}

void sendStartToA() {
  lastStatus    = "START sent to A. Onboarding, calibration, then game.";
  uiHits        = 0;
  uiMisses      = 0;
  uiRound       = 0;
  strikeCount   = 0;
  lastStrike    = "";
  gameOver      = false;
  gameRunning   = true;
  endResult     = "";
  ledMode       = LED_OFF;
  requestPanelPhase(PHASE_START);
  delay(80);
  requestPanelPhase(PHASE_START);
}

void sendAbortToA() {
  lastStatus  = "ABORT sent (manual or 3 strikes).";
  gameRunning = false;
  requestPanelPhase(PHASE_ABORT);
  delay(80);
  requestPanelPhase(PHASE_ABORT);
}

void sendResetAll() {
  lastStatus    = "RESET sent to panel/servo.";
  uiHits        = 0;
  uiMisses      = 0;
  uiRound       = 0;
  uiTotalRounds = 20;
  strikeCount   = 0;
  lastStrike    = "";
  gameOver      = false;
  gameRunning   = false;
  endResult     = "";
  ledMode       = LED_OFF;
  simActive     = false;
  simStep       = SIM_OFF;
  calibratingSent = false;
  waitingForCFinalLed = false;
  pendingFinalLedMode = LED_OFF;

  // Clear the ONBOARDING debounce so the new game is allowed to speak
  // its ONBOARDING voice line exactly once.
  lastOnboardingSentMs = 0;

  // Send RESET more than once because one board may still be booting.
  for (int i = 0; i < 3; i++) {
    requestPanelPhase(PHASE_RESET);
    sendToPeer(bMac, "B Servo",  PHASE_RESET);
    delay(120);
  }

  Serial.println("RESET_ALL");
}

void resetAllThenAutoStart() {
  sendResetAll();

  // No delay needed: A handles PHASE_RESET as a software reset
  // and automatically starts the game sequence itself.
  gameRunning = true;
  lastStatus = "D boot/reset: RESET sent. A will auto-start the game.";
}

// ============================================
// Strike handling
// ============================================
void handleStrike(const String& kind) {
  if (gameOver) {
    lastStatus = "Game already over - reset to play again.";
    return;
  }

  strikeCount++;
  lastStrike = kind;

  if (strikeCount >= 3) {
    Serial.print("STRIKE_FINAL_");
    Serial.println(kind);

    lastStatus = "3 STRIKES (" + kind + ") -> CHEAT ending.";
    endResult  = "CHEAT";
    gameOver   = true;
    gameRunning = false;

    // IMPORTANT:
    // Real in-game CHEAT must also show white flashing first,
    // not stay on rainbow animation.
    startMotorWhiteThenFinal(LED_END_CHEAT);

    sendAbortToA();
  } else {
    Serial.print("STRIKE_");
    Serial.print(kind);
    Serial.print("_");
    Serial.println(strikeCount);

    lastStatus = "Strike " + String(strikeCount) + " of 3 (" + kind + ").";
  }
}

// ============================================
// Final LED helpers
// ============================================
void startMotorWhiteThenFinal(LedMode finalMode) {
  pendingFinalLedMode = finalMode;
  waitingForCFinalLed = true;
  waitingForCStartMs  = millis();

  // White flashing while C board motors are moving.
  ledMode         = LED_CHASE;
  chaseStep       = 0;
  chaseFlashOn    = false;
  chaseFlashCount = 0;
  ledStateTs      = millis();
}

LedMode finalModeFromResult(bool success, bool cheat, int successCount, int totalRounds) {
  if (cheat) return LED_END_CHEAT;
  if (success) return LED_END_SUCCESS;
  // PERFECT and FAIL use the same red result colours.
  return LED_END_FAIL;
}

// ============================================
// ESP-NOW receive
// ============================================
void processPanelMessage(const ResultMessage &msg) {

  switch (msg.phase) {
    case PHASE_ONBOARDING: {
      // Suppress duplicates: A board may send PHASE_ONBOARDING more than
      // once during its boot / software-reset sequence. Only the first
      // one per game is forwarded to Python over Serial.
      unsigned long now = millis();
      if (lastOnboardingSentMs != 0 &&
          (now - lastOnboardingSentMs) < ONBOARDING_DEBOUNCE_MS) {
        Serial.println("# duplicate ONBOARDING suppressed");
        break;
      }
      lastOnboardingSentMs = now;
      lastStatus = "ONBOARDING";
      Serial.println("ONBOARDING");
      break;
    }

    case PHASE_COUNT_3:
      lastStatus = "COUNTDOWN 3";
      Serial.println("COUNTDOWN_3");
      break;

    case PHASE_COUNT_2:
      lastStatus = "COUNTDOWN 2";
      Serial.println("COUNTDOWN_2");
      break;

    case PHASE_COUNT_1:
      lastStatus = "COUNTDOWN 1";
      Serial.println("COUNTDOWN_1");
      break;

    case PHASE_START:
      lastStatus  = "GAME PLAYING";
      gameRunning = true;
      Serial.println("COUNTDOWN_GO");
      break;

    case PHASE_PANEL_STATUS:
      // IMPORTANT:
      // Do NOT use max() here.
      // After RESET, A board sends 0 / 0 / 0 again.
      // If max() is used, the web Live Score will stay at the old higher numbers.
      uiRound  = msg.roundCount;
      uiHits   = msg.successCount;
      uiMisses = msg.missCount;

      switch (msg.panelState) {
        case PSTATE_CALIBRATION:
          ledMode = LED_CALIBRATION;
          // Tell Python to say "Don't hit, calibrating" + count 3-2-1-Go.
          // Guard ensures it fires once per game even if A resends the status.
          if (!calibratingSent) {
            Serial.println("CALIBRATING");
            calibratingSent = true;
          }
          break;
        case PSTATE_WAIT:  ledMode = LED_WAIT_HIT; break;
        case PSTATE_HIT:   ledMode = LED_HIT;       break;
        case PSTATE_MISS:  ledMode = LED_MISS;       break;
        case PSTATE_CLEAR: ledMode = LED_OFF;        break;
      }

      ledStateTs = millis();
      break;

    case PHASE_FINISH:
      uiHits      = msg.successCount;
      uiMisses    = msg.totalRounds - msg.successCount;
      uiRound     = msg.totalRounds;
      gameRunning = false;
      gameOver    = true;
      if (msg.cheat) {
        // 3-strike abort path → CHEAT
        lastStatus = "CHEAT result";
        endResult  = "CHEAT";
        startMotorWhiteThenFinal(LED_END_CHEAT);
        Serial.println("CHEAT");
      } else if (msg.success) {
        // Score 15-19 → SUCCESS
        lastStatus = "SUCCESS result";
        endResult  = "SUCCESS";
        startMotorWhiteThenFinal(LED_END_SUCCESS);
        Serial.println("SUCCESS");
      } else if (msg.successCount == msg.totalRounds) {
        // Perfect 20/20 natural play → PERFECT (puppet ending, fail visuals)
        lastStatus = "PERFECT result";
        endResult  = "PERFECT";
        startMotorWhiteThenFinal(LED_END_FAIL);
        Serial.println("PERFECT");
      } else {
        // Score < 15 → FAIL
        lastStatus = "FAIL result";
        endResult  = "FAIL";
        startMotorWhiteThenFinal(LED_END_FAIL);
        Serial.println("FAIL");
      }
      break;

    default:
      break;
  }
}

// ============================================
// Simulation
// ============================================
void startSimulation() {
  if (simActive) return;
  simActive   = true;
  simStep     = SIM_ONBOARDING;
  simStepTs   = millis();
  simRound    = 0;
  simHits     = 0;
  simMisses   = 0;
  uiHits      = 0;
  uiMisses    = 0;
  uiRound     = 0;
  uiTotalRounds = SIM_ROUNDS;
  strikeCount = 0;
  lastStrike  = "";
  gameOver    = false;
  gameRunning = true;
  endResult   = "";
  ledMode     = LED_CALIBRATION;   // white from the very start (onboarding + countdown)

  lastStatus = "SIMULATE: onboarding";
  Serial.println("ONBOARDING");
}

void stopSimulation() {
  simActive = false;
  simStep   = SIM_OFF;
  ledMode   = LED_OFF;
}

void updateSimulation() {
  if (!simActive) return;
  if (gameOver)  { simActive = false; return; }

  unsigned long now = millis();
  unsigned long dt  = now - simStepTs;

  switch (simStep) {
    case SIM_ONBOARDING:
      // Wait 8 seconds (same as A board's onboarding voice block)
      if (dt >= 8000) {
        simStep    = SIM_CAL_WHITE;
        simStepTs  = now;
        simSubStep = 0;
        ledMode    = LED_COUNTDOWN;            // pulsing white during 3-2-1
        lastStatus = "SIMULATE: 3...";
        Serial.println("COUNTDOWN_3");
      }
      break;

    case SIM_CAL_WHITE:
      // 3 -> 2 -> 1 with 1s gaps.
      if (dt >= 1000 && simSubStep == 0) {
        Serial.println("COUNTDOWN_2");
        lastStatus = "SIMULATE: 2...";
        simSubStep = 1;
      } else if (dt >= 2000 && simSubStep == 1) {
        Serial.println("COUNTDOWN_1");
        lastStatus = "SIMULATE: 1...";
        simSubStep = 2;
      } else if (dt >= 3000) {
        simStep    = SIM_GO;
        simStepTs  = now;
        simSubStep = 0;
        Serial.println("COUNTDOWN_GO");
        ledMode    = LED_OFF;
        lastStatus = "SIMULATE: GO!";
      }
      break;

    case SIM_GO:
      if (dt >= 500) {
        simStep   = SIM_ROUND_WAIT;
        simStepTs = now;
        ledMode   = LED_WAIT_HIT;
        simRound++;
        uiRound = simRound;
        lastStatus = "SIMULATE: round " + String(simRound);
      }
      break;

    case SIM_ROUND_WAIT:
      if (dt >= 1200) {
        bool hit = (random(100) < 70);
        if (hit) { simHits++;  uiHits   = simHits;   ledMode = LED_HIT;  }
        else     { simMisses++; uiMisses = simMisses; ledMode = LED_MISS; }
        simStep   = SIM_ROUND_HIT;
        simStepTs = now;
      }
      break;

    case SIM_ROUND_HIT:
      if (dt >= 900) {
        if (simRound >= SIM_ROUNDS) {
          simStep   = SIM_DONE;
          simStepTs = now;
          ledMode   = LED_OFF;
        } else {
          simStep   = SIM_ROUND_WAIT;
          simStepTs = now;
          ledMode   = LED_WAIT_HIT;
          simRound++;
          uiRound = simRound;
        }
      }
      break;

    case SIM_DONE: {
      bool perfectResult = (simHits == SIM_ROUNDS);
      bool successResult = (!perfectResult && simHits >= (SIM_ROUNDS * 3 / 4));
      gameRunning = false;
      gameOver    = true;
      if (perfectResult)       { endResult = "PERFECT"; ledMode = LED_END_FAIL;    Serial.println("PERFECT"); }
      else if (successResult)  { endResult = "SUCCESS"; ledMode = LED_END_SUCCESS; Serial.println("SUCCESS"); }
      else                     { endResult = "FAIL";    ledMode = LED_END_FAIL;    Serial.println("FAIL"); }
      lastStatus = "SIMULATE done: " + endResult;
      simActive = false;
      break;
    }

    default:
      break;
  }
}


// ============================================
// Web page
// ============================================
String makePage() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#0b0414">
<title>Elmo's-Circus // Control</title>
<style>
  :root {
    --bg-0: #0b0414;
    --bg-1: #1a0a2e;
    --bg-2: #2a0e44;
    --accent-purple: #b14aff;
    --accent-purple-glow: rgba(177, 74, 255, 0.55);
    --accent-orange: #ff7a1a;
    --accent-orange-glow: rgba(255, 122, 26, 0.55);
    --accent-pink: #ff4d8d;
    --accent-cyan: #59f3ff;
    --good: #2ee6a6;
    --bad:  #ff4757;
    --text:    #f4e9ff;
    --text-dim: #b6a4d4;
    --line: rgba(177, 74, 255, 0.22);
  }

  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }

  html, body {
    margin: 0;
    padding: 0;
    background: var(--bg-0);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Helvetica Neue", Arial, sans-serif;
    min-height: 100vh;
    overflow-x: hidden;
  }

  body::before {
    content: '';
    position: fixed; inset: 0;
    background:
      radial-gradient(ellipse 80% 50% at 50% -10%, rgba(177,74,255,0.25), transparent 60%),
      radial-gradient(ellipse 60% 40% at 100% 100%, rgba(255,122,26,0.18), transparent 60%),
      radial-gradient(ellipse 60% 40% at 0% 100%, rgba(89,243,255,0.10), transparent 60%),
      linear-gradient(180deg, var(--bg-0) 0%, var(--bg-1) 60%, var(--bg-0) 100%);
    z-index: -2;
  }
  body::after {
    content: '';
    position: fixed; inset: 0;
    background: repeating-linear-gradient(
      0deg,
      rgba(255,255,255,0.02) 0px,
      rgba(255,255,255,0.02) 1px,
      transparent 1px,
      transparent 4px
    );
    pointer-events: none;
    z-index: -1;
  }

  .wrap { max-width: 480px; margin: 0 auto; padding: 18px 16px 32px; }

  header { text-align: center; margin: 4px 0 18px; }
  .team-badge {
    display: inline-flex; align-items: center; gap: 6px;
    background: linear-gradient(135deg, rgba(255,122,26,0.18), rgba(177,74,255,0.18));
    border: 1px solid rgba(255,122,26,0.45);
    color: var(--accent-orange);
    font-size: 10px; font-weight: 800; letter-spacing: 0.28em;
    padding: 5px 12px;
    border-radius: 999px;
    margin-bottom: 10px;
    text-shadow: 0 0 6px var(--accent-orange-glow);
    box-shadow: 0 0 14px rgba(255,122,26,0.25);
  }
  .team-pulse {
    width: 5px; height: 5px; border-radius: 50%;
    background: var(--accent-orange);
    box-shadow: 0 0 8px var(--accent-orange);
    animation: pulse 1.6s ease-in-out infinite;
  }
  .eyebrow {
    font-size: 11px; letter-spacing: 0.4em;
    color: var(--accent-orange); text-transform: uppercase;
    margin: 0 0 4px; font-weight: 700;
  }
  h1 {
    margin: 0;
    font-size: 30px;
    font-weight: 900;
    letter-spacing: 0.06em;
    background: linear-gradient(180deg, #fff 0%, var(--accent-purple) 100%);
    -webkit-background-clip: text;
    background-clip: text;
    color: transparent;
    text-shadow: 0 0 24px var(--accent-purple-glow);
  }
  .tag {
    font-size: 11px; color: var(--text-dim);
    margin: 4px 0 0; letter-spacing: 0.2em; text-transform: uppercase;
  }
  .tag .dot {
    display: inline-block; width: 6px; height: 6px; border-radius: 50%;
    background: var(--good); margin-right: 6px; box-shadow: 0 0 6px var(--good);
    animation: pulse 1.6s ease-in-out infinite;
  }
  @keyframes pulse { 50% { opacity: 0.4; } }

  .card {
    background: linear-gradient(180deg, rgba(255,255,255,0.06), rgba(255,255,255,0.02));
    border: 1px solid var(--line);
    border-radius: 18px;
    padding: 16px;
    margin: 12px 0;
    backdrop-filter: blur(6px);
    -webkit-backdrop-filter: blur(6px);
    box-shadow: 0 0 0 1px rgba(0,0,0,0.4) inset, 0 12px 40px rgba(0,0,0,0.35);
  }
  .card .card-title {
    font-size: 10px; letter-spacing: 0.3em; color: var(--text-dim);
    text-transform: uppercase; margin: 0 0 10px;
    display: flex; align-items: center; gap: 8px;
  }
  .card .card-title::before {
    content: ''; flex: 0 0 4px; height: 4px; border-radius: 50%;
    background: var(--accent-purple); box-shadow: 0 0 8px var(--accent-purple);
  }

  .stats { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 8px; margin-bottom: 10px; }
  .stat {
    text-align: center;
    background: rgba(0,0,0,0.35);
    border: 1px solid var(--line);
    border-radius: 12px;
    padding: 10px 6px 8px;
    position: relative;
    overflow: hidden;
  }
  .stat .num {
    font-size: 34px; font-weight: 900; line-height: 1;
    font-variant-numeric: tabular-nums;
    transition: transform 0.15s ease;
  }
  .stat.bump .num { transform: scale(1.18); }
  .stat .lab {
    margin-top: 4px;
    font-size: 9px; letter-spacing: 0.25em; color: var(--text-dim); text-transform: uppercase;
  }
  .stat.hits  .num { color: var(--good);          text-shadow: 0 0 16px rgba(46,230,166,0.45); }
  .stat.miss  .num { color: var(--bad);           text-shadow: 0 0 16px rgba(255,71,87,0.45); }
  .stat.round .num { color: var(--accent-purple); text-shadow: 0 0 16px var(--accent-purple-glow); }

  .strikes { display: flex; gap: 8px; justify-content: center; align-items: center; margin-top: 10px; }
  .strikes .label {
    font-size: 10px; letter-spacing: 0.25em; color: var(--text-dim);
    text-transform: uppercase; margin-right: 4px;
  }
  .x {
    width: 30px; height: 30px; border-radius: 6px;
    background: rgba(0,0,0,0.45);
    border: 1px solid var(--line);
    display: flex; align-items: center; justify-content: center;
    font-size: 13px; font-weight: 900; color: transparent;
    transition: all 0.2s ease;
  }
  .x.on.hand {
    background: linear-gradient(135deg, var(--accent-orange), #d94800);
    color: #fff; border-color: #fff;
    box-shadow: 0 0 16px var(--accent-orange-glow);
    animation: shake 0.35s ease;
  }
  .x.on.foot {
    background: linear-gradient(135deg, var(--accent-pink), #b8005c);
    color: #fff; border-color: #fff;
    box-shadow: 0 0 16px rgba(255,77,141,0.55);
    animation: shake 0.35s ease;
  }
  @keyframes shake {
    0%,100% { transform: translateX(0); }
    25% { transform: translateX(-3px) rotate(-2deg); }
    75% { transform: translateX(3px) rotate(2deg); }
  }

  button {
    -webkit-appearance: none; appearance: none;
    width: 100%;
    padding: 16px 14px;
    margin: 6px 0;
    border: 1px solid rgba(255,255,255,0.08);
    border-radius: 14px;
    color: #fff; font-size: 16px; font-weight: 800;
    letter-spacing: 0.08em; text-transform: uppercase;
    background: linear-gradient(180deg, #4a1a78, #2a0e44);
    cursor: pointer;
    transition: transform 0.08s ease, box-shadow 0.2s ease, filter 0.2s ease;
    position: relative;
    overflow: hidden;
  }
  button::after {
    content: ''; position: absolute; inset: 0;
    background: linear-gradient(180deg, rgba(255,255,255,0.18), transparent 40%);
    pointer-events: none;
  }
  button:active { transform: translateY(1px) scale(0.99); }
  button:disabled { filter: grayscale(0.6) brightness(0.55); cursor: not-allowed; }

  .btn-start {
    background: linear-gradient(180deg, #1ec98a, #0a7a52);
    box-shadow: 0 0 24px rgba(46,230,166,0.35), 0 8px 18px rgba(0,0,0,0.4);
    font-size: 19px; padding: 20px 14px;
  }
  .btn-reset {
    background: linear-gradient(180deg, #d62828, #8a1717);
    box-shadow: 0 0 18px rgba(214,40,40,0.35);
  }
  .btn-strike-hand {
    background: linear-gradient(180deg, var(--accent-orange), #c45a00);
    box-shadow: 0 0 18px var(--accent-orange-glow);
  }
  .btn-strike-foot {
    background: linear-gradient(180deg, var(--accent-pink), #a82a64);
    box-shadow: 0 0 18px rgba(255,77,141,0.4);
  }
  .btn-sim {
    background: linear-gradient(180deg, #5b21b6, #311660);
    font-size: 12px; padding: 12px;
    border-color: rgba(177,74,255,0.4);
  }
  .btn-test {
    background: linear-gradient(180deg, #3a2a55, #1f1336);
    font-size: 12px; padding: 12px;
  }

  .row { display: flex; gap: 8px; }
  .row > * { flex: 1; margin: 6px 0; }

  #endbanner {
    display: none;
    margin-top: 12px;
    padding: 18px 14px;
    border-radius: 14px;
    text-align: center;
    font-weight: 900; font-size: 22px; letter-spacing: 0.15em;
    text-transform: uppercase;
    animation: slamIn 0.45s cubic-bezier(0.34, 1.56, 0.64, 1);
    position: relative; overflow: hidden;
  }
  #endbanner::before {
    content: ''; position: absolute; inset: 0;
    background: repeating-linear-gradient(45deg,
      rgba(255,255,255,0.06) 0px, rgba(255,255,255,0.06) 6px,
      transparent 6px, transparent 12px);
    pointer-events: none;
  }
  #endbanner .label {
    font-size: 10px; letter-spacing: 0.4em; opacity: 0.8;
    margin-bottom: 4px; font-weight: 700;
  }
  .end-success { background: linear-gradient(180deg, #1ec98a, #0a7a52); box-shadow: 0 0 30px rgba(46,230,166,0.5); }
  .end-fail    { background: linear-gradient(180deg, #d62828, #6e1010); box-shadow: 0 0 30px rgba(214,40,40,0.5); }
  .end-perfect { background: linear-gradient(180deg, #d62828, #6e1010); box-shadow: 0 0 30px rgba(214,40,40,0.5); }
  .end-cheat   { background: linear-gradient(180deg, var(--accent-purple), #5b1b9a); box-shadow: 0 0 30px var(--accent-purple-glow); }
  @keyframes slamIn {
    0%   { transform: scale(0.6) rotate(-3deg); opacity: 0; }
    60%  { transform: scale(1.06) rotate(1deg); opacity: 1; }
    100% { transform: scale(1) rotate(0); }
  }

  .statusbar {
    display: flex; align-items: center; gap: 8px;
    background: rgba(0,0,0,0.45);
    border: 1px solid var(--line);
    border-radius: 10px;
    padding: 10px 12px;
    font-family: "SF Mono", Menlo, Consolas, monospace;
    font-size: 12px;
    color: var(--accent-cyan);
  }
  .statusbar .arrow { color: var(--accent-orange); }

  footer { text-align: center; margin-top: 14px; }
  footer .ip {
    font-family: "SF Mono", Menlo, monospace;
    font-size: 10px; color: var(--text-dim); letter-spacing: 0.1em;
  }
</style>
</head>
<body>
<div class="wrap">

  <header>
    <div class="team-badge"><span class="team-pulse"></span>TEAM // ELMO'S-CIRCUS</div>
    <p class="eyebrow">Elmo's Circus &mdash; Auditions</p>
    <h1>CONTROL DECK</h1>
    <p class="tag"><span class="dot"></span>A + D &middot; Strike Control</p>
  </header>

  <section class="card">
    <p class="card-title">Live Score</p>
    <div class="stats">
      <div class="stat hits"  id="statHits"><div class="num" id="hits">0</div><div class="lab">Hits</div></div>
      <div class="stat miss"  id="statMiss"><div class="num" id="misses">0</div><div class="lab">Miss</div></div>
      <div class="stat round" id="statRound"><div class="num" id="round">0</div><div class="lab">Round / <span id="total">20</span></div></div>
    </div>
    <div class="strikes">
      <span class="label">Strikes</span>
      <div class="x" id="sd1"></div>
      <div class="x" id="sd2"></div>
      <div class="x" id="sd3"></div>
    </div>
    <div id="endbanner"></div>
  </section>

  <section class="card">
    <p class="card-title">Game Controls</p>
    <div class="row">
      <button id="startGame" onclick="cmd('START')">Start</button>
      <button id="resetGame" onclick="cmd('RESET')">Reset / Restart</button>
    </div>
    <div class="row">
      <button class="btn-strike-hand" id="strikeHand" onclick="cmd('STRIKE_HAND')">Strike: Hand</button>
      <button class="btn-strike-foot" id="strikeFoot" onclick="cmd('STRIKE_FOOT')">Strike: Foot</button>
    </div>
    <div class="row">
      <button id="strikeWrong" onclick="cmd('STRIKE_WRONG')" style="width:100%;background:linear-gradient(135deg,#7c3aed,#a855f7);color:#fff;border:none;border-radius:10px;padding:14px;font-size:15px;font-weight:700;letter-spacing:0.08em;cursor:pointer;">&#9888; Strike: Wrong Panel</button>
    </div>
  </section>

  <section class="card">
    <p class="card-title">System Status</p>
    <div class="statusbar">
      <span class="arrow">&gt;</span>
      <span id="status">Ready.</span>
    </div>
  </section>

  <footer>
    <p class="ip">192.168.4.1 &middot; ElmoGame_Control</p>
    <p class="ip" style="margin-top:6px; color: var(--accent-orange); letter-spacing: 0.25em;">// ELMO'S-CIRCUS //</p>
  </footer>

</div>

<script>
let isConnected = false;
let reconnectTimer = null;
let pollTimer = null;
let failCount = 0;

function setConnectionStatus(connected) {
  isConnected = connected;
  const statusEl = document.getElementById('status');
  if (!statusEl) return;
  if (!connected) {
    statusEl.innerText = 'Reconnecting to master...';
  }
}

function resetLocalUI() {
  // Clear the web page immediately, without waiting for the next /state poll.
  // This prevents the old score from staying on the phone after Start / Reset.
  dotKinds = ["", "", ""];
  prevStrikes = 0;
  prevHits = -1;
  prevMiss = -1;
  prevRound = -1;

  document.getElementById('hits').innerText = '0';
  document.getElementById('misses').innerText = '0';
  document.getElementById('round').innerText = '0';
  document.getElementById('total').innerText = '20';
  document.getElementById('status').innerText = 'Resetting game...';

  for (let i = 1; i <= 3; i++) {
    const el = document.getElementById('sd' + i);
    el.className = 'x';
    el.innerText = '';
    el.style.background = '';
  }

  const b  = document.getElementById('endbanner');
  const bh = document.getElementById('strikeHand');
  const bf = document.getElementById('strikeFoot');
  const bw = document.getElementById('strikeWrong');
  b.style.display = 'none';
  b.className = '';
  b.innerHTML = '';
  bh.disabled = false;
  bf.disabled = false;
  bw.disabled = false;
}

function cmd(value) {
  if (value === 'RESET' || value === 'START') {
    resetLocalUI();
  }

  fetch('/cmd?value=' + value, { cache: 'no-store' })
    .then(() => {
      setConnectionStatus(true);
      pollState();   // Force one immediate refresh after command finishes.
    })
    .catch(() => setConnectionStatus(false));
}

let dotKinds   = ["","",""];
let prevStrikes = 0;
let prevHits   = -1, prevMiss = -1, prevRound = -1;

function bump(elId) {
  const el = document.getElementById(elId);
  if (!el) return;
  el.classList.add('bump');
  setTimeout(()=> el.classList.remove('bump'), 180);
}

function paintStrikes(n, lastKind) {
  if (n === 0) dotKinds = ["","",""];
  if (n > prevStrikes && n >= 1 && n <= 3) dotKinds[n-1] = lastKind;
  prevStrikes = n;

  for (let i = 1; i <= 3; i++) {
    const el = document.getElementById('sd' + i);
    el.classList.remove('on','hand','foot');
    el.innerText = '';
    if (i <= n) {
      const k = dotKinds[i-1];
      el.classList.add('on');
      if (k === 'HAND')       { el.classList.add('hand'); el.innerText = 'H'; }
      else if (k === 'FOOT')  { el.classList.add('foot'); el.innerText = 'F'; }
      else if (k === 'WRONG') { el.style.background='#a855f7'; el.innerText = 'W'; }
      else                    { el.classList.add('hand'); el.innerText = 'X'; }
    }
  }
}

function paintEnd(result) {
  const b  = document.getElementById('endbanner');
  const bh = document.getElementById('strikeHand');
  const bf = document.getElementById('strikeFoot');
  const bw = document.getElementById('strikeWrong');
  if (!result) {
    b.style.display = 'none';
    bh.disabled = false; bf.disabled = false; bw.disabled = false;
    return;
  }
  b.style.display = 'block';
  bh.disabled = true; bf.disabled = true; bw.disabled = true;
  if (result === 'SUCCESS')      { b.className = 'end-success'; b.innerHTML = '<div class="label">Verdict</div>Human (Boring)'; }
  else if (result === 'FAIL')    { b.className = 'end-fail';    b.innerHTML = '<div class="label">Verdict</div>Welcome To The Circus'; }
  else if (result === 'PERFECT') { b.className = 'end-perfect'; b.innerHTML = '<div class="label">Verdict</div>You\'re A Puppet'; }
  else if (result === 'CHEAT')   { b.className = 'end-cheat';   b.innerHTML = '<div class="label">Verdict</div>Cheating Detected'; }
}

function applyState(s) {
  if (s.hits   !== prevHits)  { document.getElementById('hits').innerText   = s.hits;   bump('statHits');  prevHits  = s.hits; }
  if (s.misses !== prevMiss)  { document.getElementById('misses').innerText = s.misses; bump('statMiss');  prevMiss  = s.misses; }
  if (s.round  !== prevRound) { document.getElementById('round').innerText  = s.round;  bump('statRound'); prevRound = s.round; }
  document.getElementById('total').innerText  = s.total;
  document.getElementById('status').innerText = s.status;
  paintStrikes(s.strikes, s.lastStrike);
  paintEnd(s.endResult);
}

function pollState() {
  fetch('/state?ts=' + Date.now(), { cache: 'no-store' })
    .then(r => {
      if (!r.ok) throw new Error('state request failed');
      return r.json();
    })
    .then(s => {
      failCount = 0;
      setConnectionStatus(true);
      applyState(s);
    })
    .catch(() => {
      failCount++;
      setConnectionStatus(false);
    });
}

function startAutoReconnect() {
  // Keep polling forever. If D board resets, Wi-Fi/server may disappear briefly,
  // but this page will automatically reconnect as soon as D comes back online.
  if (pollTimer) clearInterval(pollTimer);
  pollState();
  pollTimer = setInterval(pollState, 700);
}

window.addEventListener('load', startAutoReconnect);
</script>
</body>
</html>
)rawliteral";
  return page;
}

void handleRoot()   { server.sendHeader("Cache-Control", "no-store"); server.send(200, "text/html",  makePage()); }
void handleStatus() { server.send(200, "text/plain", lastStatus); }

void handleState() {
  String json = "{";
  json += "\"hits\":"        + String(uiHits) + ",";
  json += "\"misses\":"      + String(uiMisses) + ",";
  json += "\"round\":"       + String(uiRound) + ",";
  json += "\"total\":"       + String(uiTotalRounds) + ",";
  json += "\"strikes\":"     + String(strikeCount) + ",";
  json += "\"lastStrike\":\"" + lastStrike + "\",";
  json += "\"endResult\":\""  + endResult + "\",";
  json += "\"status\":\""    + lastStatus + "\"";
  json += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleCommand() {
  if (!server.hasArg("value")) {
    server.send(400, "text/plain", "Missing command");
    return;
  }
  String cmd = server.arg("value");
  cmd.trim();

  if (cmd == "START") {
    sendStartToA();
    lastStatus = "START requested.";
  }
  else if (cmd == "RESET") {
    // Schedule a fresh game through the existing working reset/autostart path.
    // Never run the calibration synchronously inside the HTTP handler.
    requestPanelPhase(PHASE_RESET);
    lastStatus = "Reset / restart requested.";
  }
  else if (cmd == "NO_FEET")      { Serial.println("NO_FEET"); lastStatus = "No feet warning sent to Python audio."; }
  else if (cmd == "VOL_UP")       { Serial.println("VOL_UP");  lastStatus = "Volume up command sent to Python audio."; }
  else if (cmd == "VOL_DOWN")     { Serial.println("VOL_DOWN"); lastStatus = "Volume down command sent to Python audio."; }
  else if (cmd == "STRIKE_HAND")  handleStrike("HAND");
  else if (cmd == "STRIKE_FOOT")  handleStrike("FOOT");
  else if (cmd == "STRIKE_WRONG") handleStrike("WRONG");
  else if (cmd == "SIMULATE")     { lastStatus = "SIMULATE started."; startSimulation(); }
  else if (cmd == "TEST_SUCCESS") {
    lastStatus = "Testing SUCCESS action.";
    endResult = "SUCCESS"; gameOver = true; startMotorWhiteThenFinal(LED_END_SUCCESS);
    Serial.println("SUCCESS");
  }
  else if (cmd == "TEST_FAIL") {
    lastStatus = "Testing FAIL action.";
    endResult = "FAIL"; gameOver = true; startMotorWhiteThenFinal(LED_END_FAIL);
    Serial.println("FAIL");
  }
  else if (cmd == "TEST_PERFECT") {
    lastStatus = "Testing PERFECT action (perfect score / puppet audio).";
    endResult = "PERFECT"; gameOver = true; startMotorWhiteThenFinal(LED_END_FAIL);
    Serial.println("PERFECT");
  }
  else if (cmd == "TEST_CHEAT") {
    lastStatus = "Testing CHEAT action.";
    endResult = "CHEAT"; gameOver = true; startMotorWhiteThenFinal(LED_END_CHEAT);
    Serial.println("CHEAT");
  }
  else {
    lastStatus = "Unknown command: " + cmd;
  }

  server.send(200, "text/plain", lastStatus);
}

void addPeer(uint8_t *mac, const char *name) {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  esp_err_t result = esp_now_add_peer(&peerInfo);
  Serial.print("Add peer ");
  Serial.print(name);
  Serial.print(" -> ");
  Serial.println(result == ESP_OK ? "OK" : "FAILED / already added");
}

void setup() {
  Serial.begin(115200);
  delay(800);


  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(WIFI_NAME, WIFI_PASS, 1);

  Serial.println("==================================");
  Serial.println("D Board Web Control Master + LED");
  Serial.println("==================================");
  Serial.print("A+D Master MAC: ");      Serial.println(WiFi.macAddress());
  Serial.print("Wi-Fi: ");      Serial.println(WIFI_NAME);
  Serial.print("Password: ");   Serial.println(WIFI_PASS);
  Serial.print("Open: http://"); Serial.println(WiFi.softAPIP());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  // Panel messages are local. Servo receives ESP-NOW.
  addPeer(bMac, "B Servo");

  server.on("/",       handleRoot);
  server.on("/cmd",    handleCommand);
  server.on("/status", handleStatus);
  server.on("/state",  handleState);
  server.begin();

  Serial.println("Web server ready.");

  panel_setup();

  // Exhibition mode: if D board boots or is reset, clear the whole system.
  // A board will software-reset and automatically start the game + web UI.
  resetAllThenAutoStart();
}

void loop() {
  server.handleClient();
  updateSimulation();
  panel_loop();

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "ABORT") sendAbortToA();
    if (cmd == "RESET") resetAllThenAutoStart();
    if (cmd == "START") sendStartToA();
  }
}


// Keep the strike-control website responsive while the panel game runs.
// This services web requests without changing round lengths or hit thresholds.
void panel_webYield() {
  server.handleClient();
  yield();
}
void panel_cooperativeDelay(unsigned long ms) {
  unsigned long started = millis();
  do {
    panel_webYield();
    delay(1);
  } while (millis() - started < ms);
}

// =========== INTEGRATED PANEL GAME (ORIGINAL A LOGIC) ===========
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <esp_now.h>

#define LEDS_PER_STRIP 8
#define NUM 6

#define FEEDBACK_TIME_MS 3000
#define TOTAL_ROUNDS 20

#define USE_FIXED_THRESHOLDS 1

int panel_fixedThresholds[NUM] = {
  700, 700, 700, 700, 700, 700
};

#define CALIBRATION_SAMPLES 200
#define THRESHOLD_MULTIPLIER 4
#define EXTRA_MARGIN 400
#define MIN_THRESHOLD 500
#define MAX_THRESHOLD 3000

#define WARMUP_DISCARD 10
#define WARMUP_KEEP 30
#define WARMUP_DELAY_MS 5

#define HIT_CONFIRM_COUNT 2
#define POST_HIT_COOLDOWN_MS 200
#define CROSS_PANEL_SETTLE_MS 150

#define DOUBLE_TARGET_CHANCE_PERCENT 45
#define DOUBLE_TARGET_MIN_GAP_ROUNDS 1

uint8_t panel_servoMac[]  = { 0xD0, 0xCF, 0x13, 0x52, 0x2D, 0x28 };

Adafruit_NeoPixel panel_strip0(LEDS_PER_STRIP, 14, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel panel_strip1(LEDS_PER_STRIP, 15, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel panel_strip2(LEDS_PER_STRIP, 16, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel panel_strip3(LEDS_PER_STRIP, 17, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel panel_strip4(LEDS_PER_STRIP, 18, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel panel_strip5(LEDS_PER_STRIP, 38, NEO_GRB + NEO_KHZ800);

const int PIEZO_PINS[NUM] = {2, 4, 5, 6, 7, 8};
const int LED_PINS[NUM]   = {14, 15, 16, 17, 18, 38};   // reference only

int panel_baseline[NUM];
int panel_thresholds[NUM];

int panel_lastTarget      = -1;
int panel_roundCount      = 0;
int panel_successCount    = 0;
int panel_lastDoubleRound = -10;
bool panel_gameFinished   = false;
bool panel_gameStarted    = false;
volatile bool panel_startRequested = false;
volatile bool panel_resetRequested = false;

struct panel_ResultMessage {
  uint8_t phase;
  bool    success;
  bool    cheat;
  int     panel_successCount;
  int     totalRounds;
  uint8_t panelState;
  int     missCount;
  int     panel_roundCount;
};

panel_ResultMessage panel_resultMsg;

unsigned long panel_getTargetTime() {
  if (panel_roundCount <= 5)  return 4000;
  if (panel_roundCount <= 10) return 3000;
  if (panel_roundCount <= 15) return 2000;
  return 1000;
}

void panel_showAllStrips() {
  panel_strip0.show(); panel_strip1.show(); panel_strip2.show();
  panel_strip3.show(); panel_strip4.show(); panel_strip5.show();
}

void panel_fillStrip(int stripNumber, uint32_t color) {
  for (int i = 0; i < LEDS_PER_STRIP; i++) {
    switch (stripNumber) {
      case 0: panel_strip0.setPixelColor(i, color); break;
      case 1: panel_strip1.setPixelColor(i, color); break;
      case 2: panel_strip2.setPixelColor(i, color); break;
      case 3: panel_strip3.setPixelColor(i, color); break;
      case 4: panel_strip4.setPixelColor(i, color); break;
      case 5: panel_strip5.setPixelColor(i, color); break;
    }
  }
}

void panel_clearAllStrips() {
  uint32_t black = panel_strip0.Color(0, 0, 0);
  for (int i = 0; i < NUM; i++) panel_fillStrip(i, black);
  panel_showAllStrips();
}

void panel_fillAllStrips(uint32_t color) {
  for (int i = 0; i < NUM; i++) panel_fillStrip(i, color);
  panel_showAllStrips();
}

void panel_setOnlyStrip(int target, uint32_t color) {
  uint32_t black = panel_strip0.Color(0, 0, 0);
  for (int i = 0; i < NUM; i++) panel_fillStrip(i, black);
  panel_fillStrip(target, color);
  panel_showAllStrips();
}

void panel_setTwoStrips(int tA, int tB, uint32_t color) {
  uint32_t black = panel_strip0.Color(0, 0, 0);
  for (int i = 0; i < NUM; i++) panel_fillStrip(i, black);
  panel_fillStrip(tA, color);
  panel_fillStrip(tB, color);
  panel_showAllStrips();
}

void panel_setTwoStripsResult(int tA, int tB, bool hitA, bool hitB, uint32_t green, uint32_t alt) {
  uint32_t black = panel_strip0.Color(0, 0, 0);
  for (int i = 0; i < NUM; i++) panel_fillStrip(i, black);
  panel_fillStrip(tA, hitA ? green : alt);
  panel_fillStrip(tB, hitB ? green : alt);
  panel_showAllStrips();
}

void panel_beginAllStrips() {
  panel_strip0.begin(); panel_strip1.begin(); panel_strip2.begin();
  panel_strip3.begin(); panel_strip4.begin(); panel_strip5.begin();
  panel_strip0.setBrightness(80); panel_strip1.setBrightness(80);
  panel_strip2.setBrightness(80); panel_strip3.setBrightness(80);
  panel_strip4.setBrightness(80); panel_strip5.setBrightness(80);
  panel_clearAllStrips();
}

void panel_sendToAudio(GamePhase phase) {
  panel_resultMsg.phase        = (uint8_t)phase;
  panel_resultMsg.success      = false;
  panel_resultMsg.cheat        = false;
  panel_resultMsg.panel_successCount = 0;
  panel_resultMsg.totalRounds  = TOTAL_ROUNDS;
  panel_resultMsg.panelState   = PSTATE_CLEAR;
  panel_resultMsg.missCount    = 0;
  panel_resultMsg.panel_roundCount   = panel_roundCount;
  dispatchPanelToMaster(panel_resultMsg);
}

void panel_sendPanelStatusToD(PanelState state) {
  panel_resultMsg.phase        = (uint8_t)PHASE_PANEL_STATUS;
  panel_resultMsg.success      = false;
  panel_resultMsg.cheat        = false;
  panel_resultMsg.panel_successCount = panel_successCount;
  panel_resultMsg.totalRounds  = TOTAL_ROUNDS;
  panel_resultMsg.panelState   = (uint8_t)state;
  panel_resultMsg.missCount    = (panel_roundCount > panel_successCount) ? (panel_roundCount - panel_successCount) : 0;
  panel_resultMsg.panel_roundCount   = panel_roundCount;
  dispatchPanelToMaster(panel_resultMsg);
}

void panel_onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(panel_ResultMessage)) return;

  panel_ResultMessage msg;
  memcpy(&msg, data, len);

  if (msg.phase == PHASE_RESET) {
    Serial.println("RESET received - software reset requested.");
    panel_resetRequested = true;
    panel_startRequested = false;
    panel_gameStarted = false;
    panel_gameFinished = true;
    return;
  }

  if (msg.phase == PHASE_START && !panel_gameStarted && !panel_gameFinished) {
    Serial.println("START received from D controller.");
    panel_startRequested = true;
    return;
  }

  if (msg.phase == PHASE_ABORT && !panel_gameFinished) {
    Serial.println("ABORT received - stopping game.");
    panel_gameFinished = true;
    panel_clearAllStrips();

    panel_resultMsg.phase        = (uint8_t)PHASE_ABORT;
    panel_resultMsg.success      = false;
    panel_resultMsg.cheat        = false;
    panel_resultMsg.panel_successCount = 0;
    panel_resultMsg.totalRounds  = TOTAL_ROUNDS;
    panel_resultMsg.panelState   = PSTATE_CLEAR;
    panel_resultMsg.missCount    = 0;
    panel_resultMsg.panel_roundCount   = panel_roundCount;
    esp_now_send(panel_servoMac, (uint8_t *)&panel_resultMsg, sizeof(panel_resultMsg));
    panel_cooperativeDelay(50);
    esp_now_send(panel_servoMac, (uint8_t *)&panel_resultMsg, sizeof(panel_resultMsg));

    panel_resultMsg.phase        = (uint8_t)PHASE_FINISH;
    panel_resultMsg.success      = false;
    panel_resultMsg.cheat        = true;
    panel_resultMsg.panel_successCount = panel_successCount;
    panel_resultMsg.totalRounds  = TOTAL_ROUNDS;
    panel_resultMsg.panelState   = PSTATE_CLEAR;
    panel_resultMsg.missCount    = (panel_roundCount > panel_successCount) ? (panel_roundCount - panel_successCount) : 0;
    panel_resultMsg.panel_roundCount   = panel_roundCount;

    dispatchPanelToMaster(panel_resultMsg);
    panel_cooperativeDelay(50);
    dispatchPanelToMaster(panel_resultMsg);
  }
}

void panel_useFixedThresholdsBoot() {
  Serial.println("==================================");
  Serial.println("FIXED THRESHOLD MODE");
  Serial.println("==================================");

  panel_sendPanelStatusToD(PSTATE_CALIBRATION);   // D will emit "CALIBRATING" to Python

  for (int i = 0; i < NUM; i++) {
    if (panel_gameFinished) return;

    long sum = 0;
    panel_setOnlyStrip(i, panel_strip0.Color(255, 255, 255));
    for (int j = 0; j < CALIBRATION_SAMPLES; j++) {
      sum += analogRead(PIEZO_PINS[i]);
      panel_cooperativeDelay(10);
    }
    panel_baseline[i]   = sum / CALIBRATION_SAMPLES;
    panel_thresholds[i] = panel_fixedThresholds[i];

    Serial.print("Panel "); Serial.print(i + 1);
    Serial.print(" | Baseline: "); Serial.print(panel_baseline[i]);
    Serial.print(" | Threshold: "); Serial.println(panel_thresholds[i]);

    // COUNT_3/2/1 signals removed — Python handles the full 3-2-1-Go
    // countdown internally after receiving "CALIBRATING", timed to end
    // just as the last panel finishes (~12.9 s total).

    panel_cooperativeDelay(150);
  }
  panel_clearAllStrips();
  panel_sendPanelStatusToD(PSTATE_CLEAR);
}

void panel_calibrateAndSuggestThresholds() {
  Serial.println("==================================");
  Serial.println("AUTO-CALIBRATION");
  Serial.println("==================================");

  panel_sendPanelStatusToD(PSTATE_CALIBRATION);

  for (int i = 0; i < NUM; i++) {
    if (panel_gameFinished) return;

    // Discard first readings while ADC settles
    for (int j = 0; j < 20; j++) {
      analogRead(PIEZO_PINS[i]);
      panel_cooperativeDelay(5);
    }

    long sum = 0;
    int minValue = 4095;
    int maxValue = 0;

    panel_setOnlyStrip(i, panel_strip0.Color(255, 255, 255));

    for (int j = 0; j < CALIBRATION_SAMPLES; j++) {
      int value = analogRead(PIEZO_PINS[i]);
      sum += value;
      if (value < minValue) minValue = value;
      if (value > maxValue) maxValue = value;
      panel_cooperativeDelay(10);
    }

    panel_baseline[i] = sum / CALIBRATION_SAMPLES;
    int noiseRange = maxValue - minValue;

    // Threshold = 3x the idle noise range, but at least MIN_THRESHOLD
    int t = noiseRange * THRESHOLD_MULTIPLIER + EXTRA_MARGIN;
    if (t < MIN_THRESHOLD) t = MIN_THRESHOLD;
    if (t > MAX_THRESHOLD) t = MAX_THRESHOLD;
    panel_thresholds[i] = t;

    Serial.print("Panel "); Serial.print(i + 1);
    Serial.print(" | Baseline: "); Serial.print(panel_baseline[i]);
    Serial.print(" | Noise range: "); Serial.print(noiseRange);
    Serial.print(" | Threshold: "); Serial.println(panel_thresholds[i]);

    if (i == 1) {}  // COUNT signals removed — Python owns the countdown
    if (i == 3) {}
    if (i == 4) {}

    panel_cooperativeDelay(250);
  }
  panel_clearAllStrips();
  panel_sendPanelStatusToD(PSTATE_CLEAR);
}

int panel_measureLiveBaseline(int pinIndex) {
  // Discard initial noisy readings
  for (int k = 0; k < WARMUP_DISCARD; k++) {
    (void)analogRead(PIEZO_PINS[pinIndex]);
    panel_cooperativeDelay(WARMUP_DELAY_MS);
  }
  // Average more samples for a stable panel_baseline
  long sum = 0;
  for (int k = 0; k < WARMUP_KEEP; k++) {
    sum += analogRead(PIEZO_PINS[pinIndex]);
    panel_cooperativeDelay(WARMUP_DELAY_MS);
  }
  return sum / WARMUP_KEEP;
}

void panel_endGameAnimation(bool finalResult) {
  uint32_t colorA = finalResult ? panel_strip0.Color(0, 255, 0)   : panel_strip0.Color(255, 0, 0);
  uint32_t colorB = finalResult ? panel_strip0.Color(255, 120, 0) : panel_strip0.Color(160, 0, 255);

  panel_clearAllStrips();
  panel_cooperativeDelay(300);

  for (int step = 0; step < NUM; step++) {
    for (int p = 0; p <= step; p++)
      panel_fillStrip(p, (p % 2 == 0) ? colorA : colorB);
    panel_showAllStrips();
    panel_cooperativeDelay(400);
  }

  panel_cooperativeDelay(500);

  for (int i = 0; i < 6; i++) {
    panel_fillAllStrips(colorA); panel_cooperativeDelay(180);
    panel_fillAllStrips(colorB); panel_cooperativeDelay(180);
  }

  for (int p = 0; p < NUM; p++)
    panel_fillStrip(p, (p % 2 == 0) ? colorA : colorB);
  panel_showAllStrips();
}

void panel_sendOnePeer(const uint8_t *peerMac, const char *peerName, GamePhase phase) {
  esp_err_t result = esp_now_send(peerMac, (uint8_t *)&panel_resultMsg, sizeof(panel_resultMsg));
  Serial.print("ESP-NOW Send to "); Serial.print(peerName);
  Serial.print(" ("); Serial.print(phase == PHASE_START ? "START" : "FINISH");
  Serial.print("): "); Serial.println(result == ESP_OK ? "SUCCESS" : "FAILED");
}

void panel_sendPhase(GamePhase phase, bool finalResult, bool cheatResult = false) {
  panel_resultMsg.phase        = (uint8_t)phase;
  panel_resultMsg.success      = finalResult;
  panel_resultMsg.cheat        = cheatResult;
  panel_resultMsg.panel_successCount = panel_successCount;
  panel_resultMsg.totalRounds  = TOTAL_ROUNDS;
  panel_resultMsg.panelState   = PSTATE_CLEAR;
  panel_resultMsg.missCount    = (panel_roundCount > panel_successCount) ? (panel_roundCount - panel_successCount) : 0;
  panel_resultMsg.panel_roundCount   = panel_roundCount;

  panel_sendOnePeer(panel_servoMac, "B Servo", phase);

  if (phase == PHASE_FINISH) {
    dispatchPanelToMaster(panel_resultMsg);
  }
}

void panel_sendResult(bool finalResult, bool cheatResult = false) {
  panel_sendPhase(PHASE_FINISH, finalResult, cheatResult);
}

void panel_resetGameVariablesOnly() {
  panel_roundCount = 0;
  panel_successCount = 0;
  panel_lastTarget = -1;
  panel_lastDoubleRound = -10;
  panel_gameFinished = false;
  panel_gameStarted = false;
  panel_startRequested = false;
  panel_clearAllStrips();
  panel_sendPanelStatusToD(PSTATE_CLEAR);
}

void panel_softwareResetAndAutoStart() {
  Serial.println("========== SOFTWARE RESET + AUTO START ==========");
  panel_resetGameVariablesOnly();
  panel_cooperativeDelay(100);
  panel_startGameSequence();
}

void panel_setupEspNow() {
  // WiFi AP+STA configured by combined master.
  Serial.print("This ESP32-S3 MAC: ");
  Serial.println(WiFi.macAddress());

  // ESP-NOW initialized by combined master.

  esp_now_peer_info_t peerInfo = {};
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  memcpy(peerInfo.peer_addr, panel_servoMac, 6);
  if (esp_now_add_peer(&peerInfo) != ESP_OK) { Serial.println("Failed to add B."); return; }

  Serial.println("ESP-NOW ready. Peers: B Servo + Result + D Audio.");
}

void panel_setup() {
  // Serial initialized by combined master.

  Serial.println("==================================");
  Serial.println("ESP32-S3 Pico LED + Piezo Game");
  Serial.println("==================================");

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  panel_beginAllStrips();
  panel_setupEspNow();

  randomSeed(esp_random());

  Serial.println("A board idle. Waiting for START from D web controller...");
  Serial.println();
}

void panel_startGameSequence() {
  panel_roundCount = 0;
  panel_successCount = 0;
  panel_lastTarget = -1;
  panel_lastDoubleRound = -10;
  panel_gameFinished = false;

  panel_sendToAudio(PHASE_ONBOARDING);
  // No delay — calibration starts immediately after dispatching ONBOARDING.
  // Python queues CALIBRATING behind ONBOARDING speech (~8s), then speaks
  // "Don't hit. Calibrating. 3. 2. 1. Go!" which naturally lands right as
  // the last panel finishes (~12.9 s total calibration time).

#if USE_FIXED_THRESHOLDS
  panel_useFixedThresholdsBoot();
#else
  panel_calibrateAndSuggestThresholds();
#endif

  if (panel_gameFinished) return;

  panel_cooperativeDelay(200);
  panel_sendPhase(PHASE_START, false);
  panel_cooperativeDelay(100);
  panel_sendPhase(PHASE_START, false);

  panel_gameStarted = true;
  Serial.println("Game started.");
  Serial.println();
}

void panel_loop() {
  if (panel_resetRequested) {
    panel_resetRequested = false;
    panel_softwareResetAndAutoStart();
    return;
  }

  if (!panel_gameStarted) {
    if (panel_startRequested) {
      panel_startRequested = false;
      panel_startGameSequence();
    }
    panel_cooperativeDelay(20);
    return;
  }

  if (panel_gameFinished) return;

  uint32_t blue  = panel_strip0.Color(0, 0, 255);
  uint32_t green = panel_strip0.Color(0, 255, 0);
  uint32_t red   = panel_strip0.Color(255, 0, 0);

  int targetA;
  do {
    targetA = random(0, NUM);
  } while (targetA == panel_lastTarget);

  bool doubleMode = false;
  if (panel_roundCount >= (TOTAL_ROUNDS / 4)) {
    if ((panel_roundCount - panel_lastDoubleRound) > DOUBLE_TARGET_MIN_GAP_ROUNDS &&
        random(100) < DOUBLE_TARGET_CHANCE_PERCENT) {
      doubleMode = true;
      panel_lastDoubleRound = panel_roundCount;
    }
  }

  int targetB = -1;
  if (doubleMode) {
    do {
      targetB = random(0, NUM);
    } while (targetB == targetA);
  }

  panel_lastTarget = targetA;
  panel_roundCount++;

  unsigned long targetTime = panel_getTargetTime();

  Serial.println("----------------------------------");
  Serial.print("Round: "); Serial.print(panel_roundCount);
  Serial.print(" / "); Serial.println(TOTAL_ROUNDS);
  Serial.print("Time limit: "); Serial.print(targetTime); Serial.println(" ms");

  if (doubleMode) {
    Serial.println("Mode: DOUBLE TARGET");
    Serial.print("Target Panels: ");
    Serial.print(targetA + 1); Serial.print(" + "); Serial.println(targetB + 1);
    panel_setTwoStrips(targetA, targetB, blue);
  } else {
    Serial.println("Mode: SINGLE TARGET");
    Serial.print("Target Panel: "); Serial.println(targetA + 1);
    panel_setOnlyStrip(targetA, blue);
  }

  panel_sendPanelStatusToD(PSTATE_WAIT);

  int liveBaselineA = panel_measureLiveBaseline(targetA);
  int liveBaselineB = 0;
  if (doubleMode) liveBaselineB = panel_measureLiveBaseline(targetB);

  Serial.print("Live panel_baseline A: "); Serial.print(liveBaselineA);
  if (doubleMode) { Serial.print("  B: "); Serial.print(liveBaselineB); }
  Serial.println();

  unsigned long start = millis();
  bool hitA = false;
  bool hitB = !doubleMode;
  int consecutiveHighA = 0;
  int consecutiveHighB = 0;
  unsigned long hitATime = 0;
  unsigned long hitBTime = 0;

  while (millis() - start < targetTime) {
    panel_webYield();
    if (panel_gameFinished) { panel_clearAllStrips(); return; }

    unsigned long now = millis();

    if (!hitA) {
      bool bJustHit = (hitBTime > 0) && (now - hitBTime < CROSS_PANEL_SETTLE_MS);
      if (!bJustHit) {
        int valueA = analogRead(PIEZO_PINS[targetA]);
        int deltaA = abs(valueA - liveBaselineA);
        if (deltaA > panel_thresholds[targetA]) {
          consecutiveHighA++;
          if (consecutiveHighA >= HIT_CONFIRM_COUNT) {
            hitA = true;
            hitATime = now;
            Serial.print("Panel "); Serial.print(targetA + 1);
            Serial.print(" HIT (delta="); Serial.print(deltaA); Serial.println(")");
          }
        } else {
          consecutiveHighA = 0;
        }
      }
    }

    if (doubleMode && !hitB) {
      bool aJustHit = (hitATime > 0) && (now - hitATime < CROSS_PANEL_SETTLE_MS);
      if (!aJustHit) {
        int valueB = analogRead(PIEZO_PINS[targetB]);
        int deltaB = abs(valueB - liveBaselineB);
        if (deltaB > panel_thresholds[targetB]) {
          consecutiveHighB++;
          if (consecutiveHighB >= HIT_CONFIRM_COUNT) {
            hitB = true;
            hitBTime = now;
            Serial.print("Panel "); Serial.print(targetB + 1);
            Serial.print(" HIT (delta="); Serial.print(deltaB); Serial.println(")");
          }
        } else {
          consecutiveHighB = 0;
        }
      }
    }

    if (hitA && hitB) break;

    panel_cooperativeDelay(5);
  }

  if (panel_gameFinished) { panel_clearAllStrips(); return; }

  bool roundSuccess = hitA && hitB;

  if (roundSuccess) {
    panel_successCount++;
    Serial.println("Result: Correct hit");
    if (doubleMode) {
      panel_setTwoStrips(targetA, targetB, green);
    } else {
      panel_setOnlyStrip(targetA, green);
    }
    panel_sendPanelStatusToD(PSTATE_HIT);
    panel_cooperativeDelay(POST_HIT_COOLDOWN_MS);
  } else {
    Serial.println("Result: Missed");
    if (doubleMode) {
      panel_setTwoStrips(targetA, targetB, red);
    } else {
      panel_setOnlyStrip(targetA, red);
    }
    panel_sendPanelStatusToD(PSTATE_MISS);
  }

  Serial.print("Successful Hits: "); Serial.println(panel_successCount);

  panel_cooperativeDelay(FEEDBACK_TIME_MS);
  panel_clearAllStrips();
  panel_sendPanelStatusToD(PSTATE_CLEAR);
  panel_cooperativeDelay(200);

  if (panel_roundCount >= TOTAL_ROUNDS) {
    // PERFECT = 20/20 natural play → its own state (NOT cheat).
    // CHEAT only ever arrives via 3-strike ABORT, never at natural game end.
    bool perfectResult = (panel_successCount == TOTAL_ROUNDS);
    bool finalResult   = (!perfectResult && panel_successCount >= 15 && panel_successCount <= 19);

    Serial.println("========== GAME FINISHED ==========");
    Serial.print("Final Score: "); Serial.print(panel_successCount);
    Serial.print(" / "); Serial.println(TOTAL_ROUNDS);

    if (perfectResult)    Serial.println("GAME RESULT: PERFECT (puppet)");
    else if (finalResult) Serial.println("GAME RESULT: SUCCESS");
    else                  Serial.println("GAME RESULT: FAIL");

    // Always send cheat=false here; D detects PERFECT via panel_successCount==totalRounds.
    panel_sendResult(finalResult, false);
    panel_cooperativeDelay(100);
    panel_sendResult(finalResult, false);
    panel_cooperativeDelay(100);
    panel_sendResult(finalResult, false);
    panel_cooperativeDelay(100);
    panel_sendResult(finalResult, false);

    panel_endGameAnimation(finalResult);
    panel_gameFinished = true;
  }
}

void dispatchPanelToMaster(const panel_ResultMessage &p) {
  ResultMessage m;
  static_assert(sizeof(m) == sizeof(p), "Protocol layouts differ");
  memcpy(&m, &p, sizeof(m));
  processPanelMessage(m);
}
void requestPanelPhase(GamePhase phase) {
  panel_ResultMessage m = {};
  m.phase = (uint8_t)phase;
  panel_onReceive(nullptr, (const uint8_t *)&m, sizeof(m));
}
