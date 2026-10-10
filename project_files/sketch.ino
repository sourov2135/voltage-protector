
#include <Arduino.h>
#include <EEPROM.h>
#include <LiquidCrystal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ---------- hardware map (matches diagram.json) ----------
#define RELAY_ACTIVE_HIGH 1          // set to 0 if your relay module is active-low
const uint8_t PIN_V         = A0;
const uint8_t PIN_I         = A1;
const uint8_t PIN_BTN_SEL   = 8;
const uint8_t PIN_BTN_UP    = 9;
const uint8_t PIN_BTN_DN    = 10;
const uint8_t PIN_RELAY     = 7;
const uint8_t PIN_LED_FAULT = 6;
const uint8_t PIN_LED_OK    = 13;
const uint8_t PIN_BUZZ      = A2;
LiquidCrystal lcd(12, 11, 5, 4, 3, 2);   // RS, E, D4, D5, D6, D7

// ---------- constants ----------
const float    V_FULL_SCALE   = 300.0f;  // volts at ADC = 1023
const float    I_FULL_SCALE   = 20.0f;   // amps  at ADC = 1023
const int      HYST_V         = 5;       // volts of hysteresis for reconnect
const int      SURGE_MARGIN   = 30;      // volts above OV that trips instantly
const float    SC_FACTOR      = 3.0f;    // x over-current limit = short circuit
const float    SC_MAX_A       = 19.0f;   // keep the short-circuit level reachable on a 20 A scale
const float    OC_T2X_MS      = 1000.0f; // trip time at 2x overload
const uint16_t SCAN_MS        = 10;      // protection scan period
const uint32_t PAGE_TIMEOUT   = 15000UL; // menu returns to status page

// ---------- settings (stored in EEPROM) ----------
enum Key : uint8_t { K_OV, K_UV, K_OC, K_TDLY, K_RDLY };
struct Settings {
  uint8_t magic;
  int16_t ov;     // over-voltage limit, V
  int16_t uv;     // under-voltage limit, V
  int16_t oc10;   // over-current limit, tenths of A
  int16_t tdly;   // voltage trip delay, ms
  int16_t rdly;   // reconnect delay, s
};
const uint8_t MAGIC = 0x5A;
Settings cfg;

bool cfgValid(const Settings &s) {
  return s.magic == MAGIC &&
         s.ov >= 200 && s.ov <= 300 &&
         s.uv >= 100 && s.uv <= 220 &&
         (s.ov - s.uv) >= 20 &&
         s.oc10 >= 10 && s.oc10 <= 200 &&
         s.tdly >= 50 && s.tdly <= 2000 &&
         s.rdly >= 1 && s.rdly <= 60;
}
void loadDefaults() {
  cfg.magic = MAGIC; cfg.ov = 250; cfg.uv = 180;
  cfg.oc10 = 50; cfg.tdly = 200; cfg.rdly = 5;
}
void saveCfg() { EEPROM.put(0, cfg); }
void loadCfg() {
  EEPROM.get(0, cfg);
  if (!cfgValid(cfg)) { loadDefaults(); saveCfg(); }
}
bool trySet(uint8_t key, long val) {
  if (val < -30000L || val > 30000L) return false;
  Settings t = cfg;
  switch (key) {
    case K_OV:   t.ov   = (int16_t)val; break;
    case K_UV:   t.uv   = (int16_t)val; break;
    case K_OC:   t.oc10 = (int16_t)val; break;
    case K_TDLY: t.tdly = (int16_t)val; break;
    case K_RDLY: t.rdly = (int16_t)val; break;
    default: return false;
  }
  if (!cfgValid(t)) return false;
  cfg = t;
  saveCfg();
  return true;
}
void adjust(uint8_t key, int dir) {
  long cur, step;
  switch (key) {
    case K_OV:   cur = cfg.ov;   step = 5;  break;
    case K_UV:   cur = cfg.uv;   step = 5;  break;
    case K_OC:   cur = cfg.oc10; step = 5;  break;
    case K_TDLY: cur = cfg.tdly; step = 50; break;
    default:     cur = cfg.rdly; step = 1;  break;
  }
  trySet(key, cur + dir * step);
}

// ---------- protection state ----------
enum State : uint8_t { ST_WAIT, ST_ON, ST_LATCHED };
enum Cause : uint8_t { C_NONE, C_OV, C_UV, C_OC, C_SC, C_BOOT };
State    state = ST_WAIT;
Cause    lastCause = C_BOOT;
bool     relayOn = false;
float    volts = 0, amps = 0;
float    heat = 0;
bool     ovArm = false, uvArm = false;
uint32_t ovStart = 0, uvStart = 0, waitStart = 0;
uint32_t buzzUntil = 0, tScan = 0, tLcd = 0, tLog = 0, tLastKey = 0;
bool     buzzOn = false, logOn = false;
uint8_t  page = 0;               // 0 = status, 1..5 = settings pages

const char *causeName(uint8_t c) {
  switch (c) {
    case C_OV: return "OV";
    case C_UV: return "UV";
    case C_OC: return "OC";
    case C_SC: return "SC";
    case C_BOOT: return "BOOT";
    default: return "NONE";
  }
}
const char *stateName() {
  return state == ST_ON ? "ON" : (state == ST_WAIT ? "WAIT" : "LATCHED");
}
void stamp() { Serial.print('['); Serial.print(millis()); Serial.print(F(" ms] ")); }

void setRelay(bool on) {
  relayOn = on;
  digitalWrite(PIN_RELAY, (on == (RELAY_ACTIVE_HIGH != 0)) ? HIGH : LOW);
}
void connectLoad() {
  setRelay(true);
  state = ST_ON; heat = 0; ovArm = false; uvArm = false;
  stamp(); Serial.println(F("RELAY ON  - load connected"));
}
void trip(uint8_t c, bool latch, uint32_t now) {
  setRelay(false);
  lastCause = (Cause)c; heat = 0; ovArm = false; uvArm = false;
  state = latch ? ST_LATCHED : ST_WAIT;
  waitStart = now; buzzUntil = now + 400;
  stamp();
  Serial.print(F("TRIP ")); Serial.print(causeName(c));
  Serial.print(F("  V=")); Serial.print(volts, 1);
  Serial.print(F("  I=")); Serial.print(amps, 1);
  Serial.println(latch ? F("  -> relay OFF, LATCHED (send RESET)")
                       : F("  -> relay OFF, auto-reconnect when safe"));
}
void doReset(uint32_t now) {
  if (state != ST_LATCHED) { Serial.println(F("Nothing to reset.")); return; }
  state = ST_WAIT; waitStart = now; heat = 0;
  stamp(); Serial.println(F("RESET - waiting reconnect delay"));
}

float readScaled(uint8_t pin, float fullScale) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += analogRead(pin);
  return (sum / 8.0f) * fullScale / 1023.0f;
}

void protect(uint32_t now, uint32_t dt) {
  const float oc = cfg.oc10 / 10.0f;
  if (state == ST_ON) {
    if (volts > cfg.ov + SURGE_MARGIN) { trip(C_OV, false, now); return; }
    if (volts > cfg.ov) {
      if (!ovArm) { ovArm = true; ovStart = now; }
      else if (now - ovStart >= (uint32_t)cfg.tdly) { trip(C_OV, false, now); return; }
    } else ovArm = false;
    if (volts < cfg.uv) {
      if (!uvArm) { uvArm = true; uvStart = now; }
      else if (now - uvStart >= (uint32_t)cfg.tdly) { trip(C_UV, false, now); return; }
    } else uvArm = false;

    float sc = oc * SC_FACTOR;
    if (sc > SC_MAX_A) sc = SC_MAX_A;
    if (amps > sc) { trip(C_SC, true, now); return; }
    if (amps > oc) {
      float r = amps / oc;
      heat += (r * r - 1.0f) * (float)dt;
      if (heat >= OC_T2X_MS * 3.0f) { trip(C_OC, true, now); return; }
    } else {
      heat -= 0.5f * (float)dt;
      if (heat < 0) heat = 0;
    }
  } else if (state == ST_WAIT) {
    bool ok = (volts <= cfg.ov - HYST_V) && (volts >= cfg.uv + HYST_V);
    if (!ok) waitStart = now;
    else if (now - waitStart >= (uint32_t)cfg.rdly * 1000UL) connectLoad();
  }
}

// ---------- buttons ----------
struct Btn { uint8_t pin; bool raw, stable, repeat; uint32_t tChange, tRepeat; };
Btn bSel = {PIN_BTN_SEL, false, false, false, 0, 0};
Btn bUp  = {PIN_BTN_UP,  false, false, true,  0, 0};
Btn bDn  = {PIN_BTN_DN,  false, false, true,  0, 0};

bool btnEvent(uint8_t pin, bool &rawState, bool &stableState, bool repeat,
              uint32_t &tChange, uint32_t &tRepeat, uint32_t now) {
  bool raw = (digitalRead(pin) == LOW);
  if (raw != rawState) { rawState = raw; tChange = now; }
  if (now - tChange >= 30 && raw != stableState) {
    stableState = raw;
    if (raw) { tRepeat = now + 500; return true; }
    return false;
  }
  if (repeat && stableState && (int32_t)(now - tRepeat) >= 0) {
    tRepeat = now + 120;
    return true;
  }
  return false;
}
const uint8_t PAGE_KEY[] = {0, K_OV, K_UV, K_OC, K_TDLY, K_RDLY};
void handleButtons(uint32_t now) {
  if (btnEvent(bSel.pin, bSel.raw, bSel.stable, bSel.repeat, bSel.tChange, bSel.tRepeat, now)) { page = (page + 1) % 6; tLastKey = now; tLcd = 0; }
  int dir = 0;
  if (btnEvent(bUp.pin, bUp.raw, bUp.stable, bUp.repeat, bUp.tChange, bUp.tRepeat, now)) dir = +1;
  if (btnEvent(bDn.pin, bDn.raw, bDn.stable, bDn.repeat, bDn.tChange, bDn.tRepeat, now)) dir = -1;
  if (dir != 0) {
    tLastKey = now; tLcd = 0;
    if (page == 0) { if (dir > 0) doReset(now); }
    else adjust(PAGE_KEY[page], dir);
  }
  if (page != 0 && now - tLastKey > PAGE_TIMEOUT) page = 0;
}

// ---------- LCD ----------
void lcdLine(uint8_t row, const char *t) {
  lcd.setCursor(0, row);
  uint8_t n = 0;
  while (t[n] && n < 16) { lcd.write(t[n]); n++; }
  while (n < 16) { lcd.write(' '); n++; }
}
void updateLcd(uint32_t now) {
  if (now - tLcd < 200) return;
  tLcd = now;
  char a[24], b[24];
  if (page == 0) {
    int v = (int)(volts + 0.5f);
    int i10 = (int)(amps * 10.0f + 0.5f);
    snprintf(a, sizeof(a), "V:%3d  I:%2d.%dA", v, i10 / 10, i10 % 10);
    if (state == ST_ON) {
      snprintf(b, sizeof(b), "ON  load OK");
    } else if (state == ST_WAIT) {
      long rem = (long)cfg.rdly - (long)((now - waitStart) / 1000UL);
      if (rem < 0) rem = 0;
      snprintf(b, sizeof(b), "OFF %s WAIT %lds", causeName(lastCause), rem);
    } else {
      snprintf(b, sizeof(b), "%s LATCH UP=RST", causeName(lastCause));
    }
  } else {
    const char *names[] = {"", "Over-volt", "Under-volt", "Over-curr", "Trip delay", "Reconnect"};
    snprintf(a, sizeof(a), "SET: %s", names[page]);
    switch (page) {
      case 1: snprintf(b, sizeof(b), "%d V  (UP/DN)", cfg.ov); break;
      case 2: snprintf(b, sizeof(b), "%d V  (UP/DN)", cfg.uv); break;
      case 3: snprintf(b, sizeof(b), "%d.%d A (UP/DN)", cfg.oc10 / 10, cfg.oc10 % 10); break;
      case 4: snprintf(b, sizeof(b), "%d ms (UP/DN)", cfg.tdly); break;
      default: snprintf(b, sizeof(b), "%d s  (UP/DN)", cfg.rdly); break;
    }
  }
  lcdLine(0, a); lcdLine(1, b);
}

// ---------- indicators ----------
void updateOutputs(uint32_t now) {
  digitalWrite(PIN_LED_OK, relayOn ? HIGH : LOW);
  bool red = false;
  if (state == ST_LATCHED) red = true;
  else if (state == ST_WAIT && lastCause != C_BOOT) red = ((now / 250) % 2) == 0;
  digitalWrite(PIN_LED_FAULT, red ? HIGH : LOW);

  bool want = false;
  if (state == ST_LATCHED) want = ((now / 500) % 2) == 0;
  else if ((int32_t)(buzzUntil - now) > 0) want = true;
  if (want && !buzzOn) { tone(PIN_BUZZ, 2000); buzzOn = true; }
  else if (!want && buzzOn) { noTone(PIN_BUZZ); buzzOn = false; }
}

// ---------- serial interface ----------
void printSettings() {
  Serial.print(F("OV="));   Serial.print(cfg.ov);   Serial.print(F(" V  "));
  Serial.print(F("UV="));   Serial.print(cfg.uv);   Serial.print(F(" V  "));
  Serial.print(F("OC="));   Serial.print(cfg.oc10 / 10.0f, 1); Serial.print(F(" A  "));
  Serial.print(F("TDLY=")); Serial.print(cfg.tdly); Serial.print(F(" ms  "));
  Serial.print(F("RDLY=")); Serial.print(cfg.rdly); Serial.println(F(" s"));
}
void printStatus() {
  Serial.print(F("STATE=")); Serial.print(stateName());
  Serial.print(F("  RELAY=")); Serial.print(relayOn ? F("ON") : F("OFF"));
  Serial.print(F("  V=")); Serial.print(volts, 1);
  Serial.print(F("  I=")); Serial.print(amps, 1);
  Serial.print(F("  LAST_FAULT=")); Serial.println(causeName(lastCause));
}
void printHelp() {
  Serial.println(F("Commands (case-insensitive, end with Enter):"));
  Serial.println(F("  STATUS             show state, voltage, current"));
  Serial.println(F("  GET                show settings"));
  Serial.println(F("  SET OV <200-300>   over-voltage limit (V)"));
  Serial.println(F("  SET UV <100-220>   under-voltage limit (V), OV-UV >= 20"));
  Serial.println(F("  SET OC <1-20>      over-current limit (A, e.g. 7.5)"));
  Serial.println(F("  SET TDLY <50-2000> voltage trip delay (ms)"));
  Serial.println(F("  SET RDLY <1-60>    reconnect delay (s)"));
  Serial.println(F("  RESET              clear latched over-current fault"));
  Serial.println(F("  DEFAULTS           restore factory settings"));
  Serial.println(F("  LOG ON|OFF         1 Hz measurement stream"));
}
void processLine(char *line, uint32_t now) {
  for (char *p = line; *p; ++p) *p = toupper((unsigned char)*p);
  char *cmd = strtok(line, " \t");
  if (!cmd) return;
  if (!strcmp(cmd, "HELP")) printHelp();
  else if (!strcmp(cmd, "STATUS")) printStatus();
  else if (!strcmp(cmd, "GET")) printSettings();
  else if (!strcmp(cmd, "RESET")) doReset(now);
  else if (!strcmp(cmd, "DEFAULTS")) {
    loadDefaults(); saveCfg();
    Serial.println(F("OK defaults restored")); printSettings();
  }
  else if (!strcmp(cmd, "LOG")) {
    char *a = strtok(NULL, " \t");
    if (a && !strcmp(a, "ON")) { logOn = true; Serial.println(F("OK log on")); }
    else if (a && !strcmp(a, "OFF")) { logOn = false; Serial.println(F("OK log off")); }
    else Serial.println(F("ERR usage: LOG ON|OFF"));
  }
  else if (!strcmp(cmd, "SET")) {
    char *k = strtok(NULL, " \t");
    char *v = strtok(NULL, " \t");
    if (!k || !v) { Serial.println(F("ERR usage: SET <OV|UV|OC|TDLY|RDLY> <value>")); return; }
    int key = -1; long val = 0;
    if (!strcmp(k, "OV")) { key = K_OV; val = atol(v); }
    else if (!strcmp(k, "UV")) { key = K_UV; val = atol(v); }
    else if (!strcmp(k, "OC")) { key = K_OC; val = (long)(atof(v) * 10.0 + 0.5); }
    else if (!strcmp(k, "TDLY")) { key = K_TDLY; val = atol(v); }
    else if (!strcmp(k, "RDLY")) { key = K_RDLY; val = atol(v); }
    if (key < 0) { Serial.println(F("ERR unknown setting")); return; }
    if (trySet(key, val)) { Serial.print(F("OK ")); printSettings(); tLcd = 0; }
    else Serial.println(F("ERR value out of range or breaks OV-UV >= 20 rule"));
  }
  else Serial.println(F("ERR unknown command, type HELP"));
}
char rxBuf[40]; uint8_t rxLen = 0;
void handleSerial(uint32_t now) {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxLen > 0) { rxBuf[rxLen] = 0; processLine(rxBuf, now); rxLen = 0; }
    } else if (rxLen < sizeof(rxBuf) - 1) rxBuf[rxLen++] = c;
    else rxLen = 0;   // line too long: discard
  }
}
void streamLog(uint32_t now) {
  if (!logOn || now - tLog < 1000) return;
  tLog = now;
  Serial.print(F("LOG t=")); Serial.print(now / 1000);
  Serial.print(F("s V=")); Serial.print(volts, 1);
  Serial.print(F(" I=")); Serial.print(amps, 1);
  Serial.print(F(" STATE=")); Serial.println(stateName());
}

// ---------- Arduino entry points ----------
void setup() {
  pinMode(PIN_RELAY, OUTPUT); setRelay(false);
  pinMode(PIN_LED_FAULT, OUTPUT); pinMode(PIN_LED_OK, OUTPUT);
  pinMode(PIN_BUZZ, OUTPUT);
  pinMode(PIN_BTN_SEL, INPUT_PULLUP);
  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DN, INPUT_PULLUP);
  Serial.begin(9600);
  lcd.begin(16, 2);
  loadCfg();
  volts = readScaled(PIN_V, V_FULL_SCALE);
  amps  = readScaled(PIN_I, I_FULL_SCALE);
  waitStart = millis();
  lcdLine(0, "Voltage Protect"); lcdLine(1, "Mostofa Tanvir Sourov");
  Serial.println(F("\n=== Voltage Protector ready (type HELP) ==="));
  printSettings();
  Serial.println(F("Power-up delay running, load is OFF until voltage is safe."));
  delay(800);
}

void loop() {
  uint32_t now = millis();
  if (now - tScan >= SCAN_MS) {
    uint32_t dt = now - tScan;
    tScan = now;
    volts = readScaled(PIN_V, V_FULL_SCALE);
    amps  = readScaled(PIN_I, I_FULL_SCALE);
    protect(now, dt);
  }
  handleButtons(now);
  handleSerial(now);
  updateOutputs(now);
  updateLcd(now);
  streamLog(now);
}