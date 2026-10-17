/*
 * Darkroom Multi-Timer (Dev / Stop / Fix) v1.3
 * Copyright (c) 2026 Kobe Vermeire
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 * External Libraries Used:
 * - TM1637Display (by Avishay Orpaz) - Licensed under GNU Lesser General Public License v3.0 (LGPL-3.0)
 * - Encoder (by Paul Stoffregen)      - Licensed under MIT License
 */

// Darkroom Multi-Timer (Dev / Stop / Fix)
// TM1637 4-digit displays + 3 rotary encoders (with push) + piezo
//
// Bediening
//  - Draai aan een encoder om de tijd in te stellen (enkel als die timer NIET loopt).
//  - Klik om de timer te STARTEN. Klik tijdens het lopen = stop & reset.
//  - Lang drukken (>=1.2s) in rust = ingestelde tijd bewaren in EEPROM.
//  - Als een timer afloopt: 3 piepjes + display knippert "00:00".
//
//  Nieuw in v1.2
//  - DEV-encoder dubbelklikken  -> helderheid-modus ("br 1".."br 8")
//      draaien = helderheid live aanpassen (alle 3 displays)
//      klikken = bewaren in EEPROM en terug naar normaal
//  - STOP-encoder dubbelklikken -> volume-modus ("uo 0".."uo 8", 0 = stil)
//      draaien = volume aanpassen (korte testpiep bij elke stap)
//      klikken = bewaren in EEPROM en terug naar normaal
//  - 20 s niets doen in een instelmodus = verlaten ZONDER bewaren (oude waarde terug).
//  - Alles wat er gebeurt wordt naar de seriële output gestuurd (9600 baud).
//  - Let op: op DEV en STOP start een enkele klik ~0,4 s later, omdat de
//    firmware eerst wacht of er een tweede klik (dubbelklik) volgt.
//
//  Nieuw in v1.3
//  - Bij elke druk op een encoder flikkert dat display kort (bevestiging van de klik).
//
// Volume: tone() kent geen volume. Daarom wordt de piëzo nu via Timer1 aangestuurd
// met een variabele duty-cycle (smallere puls = stiller). Werkt met een PASSIEVE piëzo.
//
//
// Bedrading (ATmega32U4: Pro Micro / Leonardo) => kies Arduino Leonardo als board in de IDE
//   Displays (CLK,DIO): DEV 14,15 | STOP 10,16 | FIX 9,8
//   Encoders (A,B,SW) : DEV 3,20,18 | STOP 7,21,5 | FIX 2,4,6   (SW met INPUT_PULLUP)
//   Piëzo             : A1 (met serieweerstand ~100Ω)
//   Alle massa's gemeenschappelijk.

#include <TM1637Display.h>
#include <Encoder.h>
#include <EEPROM.h>

const char* firmwareVersion = "1.3";

// --- Displays --- TM1637Display(clk, dio)
TM1637Display dispDev(14, 15);
TM1637Display dispStop(10, 16);
TM1637Display dispFix(9, 8);

// --- Encoders ---
Encoder encDev(3, 20);
Encoder encStop(7, 21);
Encoder encFix(2, 4);

// --- Encoder push pins ---
const int swDev  = 18;
const int swStop = 5;
const int swFix  = 6;

// --- Buzzer ---
const int buzzerPin = A1;

// --- Defaults in seconds ---
const uint16_t DEF_DEV  = 180;  // 3 min
const uint16_t DEF_STOP =  60;  // 1 min
const uint16_t DEF_FIX  = 300;  // 5 min

// --- EEPROM addresses ---
const int ADDR_DEV    = 0;
const int ADDR_STOP   = 2;
const int ADDR_FIX    = 4;
const int ADDR_BRIGHT = 6;  // 1 byte, 0..7
const int ADDR_VOLUME = 7;  // 1 byte, 0..MAX_VOLUME

// --- Helderheid & volume ---
const uint8_t MAX_BRIGHT = 7;  // TM1637: 0..7 (getoond als 1..8)
const uint8_t DEF_BRIGHT = 7;
const uint8_t MAX_VOLUME = 8;  // 0 = stil
const uint8_t DEF_VOLUME = 8;
// Duty-cycle per volumestap, in promille (500 = 50% = maximaal volume)
const uint16_t VOL_DUTY[MAX_VOLUME + 1] = { 0, 4, 8, 16, 30, 60, 120, 250, 500 };

uint8_t brightness  = DEF_BRIGHT;
uint8_t volumeLevel = DEF_VOLUME;

// --- Instelmodi ---
enum SettingMode : uint8_t { MODE_NORMAL, MODE_BRIGHT, MODE_VOLUME };
const SettingMode DOUBLE_CLICK_MODE[3] = { MODE_BRIGHT, MODE_VOLUME, MODE_NORMAL };
SettingMode mode[3] = { MODE_NORMAL, MODE_NORMAL, MODE_NORMAL };
uint8_t settingOrig[3];
unsigned long settingActivity[3];

const unsigned long DOUBLE_CLICK_MS     = 400;
const unsigned long SETTINGS_TIMEOUT_MS = 20000;

// Segmenten voor letters
const uint8_t SEG_LOW_B = SEG_C | SEG_D | SEG_E | SEG_F | SEG_G; // b
const uint8_t SEG_LOW_R = SEG_E | SEG_G;                         // r
const uint8_t SEG_LOW_U = SEG_C | SEG_D | SEG_E;                 // u
const uint8_t SEG_LOW_O = SEG_C | SEG_D | SEG_E | SEG_G;         // o

struct TimerState {
  uint16_t setSecs;
  uint16_t remain;
  bool running;
  unsigned long lastTick;
  long lastEncPos;
  bool lastBtn;
  unsigned long lastBtnChange;
  unsigned long pressStartMs;
  bool clickPending;          // eerste klik gezien, wachten op mogelijke tweede
  unsigned long clickAt;
};

TimerState T[3];
TM1637Display* D[3] = { &dispDev, &dispStop, &dispFix };
Encoder*       E[3] = { &encDev,  &encStop,  &encFix  };
const int      SW[3] = { swDev, swStop, swFix };
const char*    NAMES[3] = { "DEV", "STOP", "FIX" };

bool finished[3] = {false, false, false};
unsigned long finishedAt[3] = {0, 0, 0};

const unsigned long LONG_PRESS_MS = 1200;
const int ENC_STEP = 5; // seconds per encoder step

// --- Effect states (non-blocking) ---
struct EffectState {
  bool active = false;
  int step = 0;
  unsigned long lastChange = 0;
};

EffectState beepFx[3];
EffectState flashFx[3];
EffectState saveFx[3];    // short confirmation beep for EEPROM save
EffectState cancelFx[3];  // cancel/reset beep
EffectState previewFx;    // testpiep bij volume-aanpassing
EffectState clickFx[3];   // kort flikkeren van het display bij een klik

const unsigned long CLICK_FLASH_MS = 100;

// =====================================================================
// Piëzo-aansturing met volume (Timer1, CTC-mode, duty-cycle via OCR1B)
// =====================================================================
volatile uint8_t* buzPort;
uint8_t buzMask;

ISR(TIMER1_COMPA_vect) { *buzPort |= buzMask; }   // begin periode: hoog
ISR(TIMER1_COMPB_vect) { *buzPort &= ~buzMask; }  // na duty: laag

void buzzerOff() {
  uint8_t s = SREG; cli();
  TIMSK1 = 0;
  TCCR1B = 0;
  *buzPort &= ~buzMask;
  SREG = s;
}

void buzzerTone(uint16_t freq) {
  if (volumeLevel == 0 || freq == 0) { buzzerOff(); return; }
  uint32_t period = 2000000UL / freq;          // 16 MHz / prescaler 8 = 2 MHz
  if (period > 65535UL) period = 65535UL;
  if (period < 20) period = 20;
  uint32_t high = period * VOL_DUTY[volumeLevel] / 1000UL;
  if (high < 4) high = 4;

  uint8_t s = SREG; cli();
  TCCR1B = 0;
  TCCR1A = 0;
  TCNT1  = 0;
  OCR1A  = (uint16_t)(period - 1);
  OCR1B  = (uint16_t)high;
  TIFR1  = _BV(OCF1A) | _BV(OCF1B);
  TIMSK1 = _BV(OCIE1A) | _BV(OCIE1B);
  *buzPort |= buzMask;
  TCCR1B = _BV(WGM12) | _BV(CS11);             // CTC, prescaler 8
  SREG = s;
}

// =====================================================================
// Utility functions
// =====================================================================
void showTime(TM1637Display& d, uint16_t secs, bool colon) {
  uint8_t digits[4];
  uint16_t m = secs / 60;
  uint16_t s = secs % 60;
  digits[0] = d.encodeDigit((m / 10) % 10);
  digits[1] = d.encodeDigit(m % 10);
  digits[2] = d.encodeDigit((s / 10) % 10);
  digits[3] = d.encodeDigit(s % 10);
  if (colon) digits[1] |= 0x80;
  d.setSegments(digits);
}

void showSetting(TM1637Display& d, uint8_t l1, uint8_t l2, uint8_t value) {
  uint8_t digits[4] = { l1, l2, 0, d.encodeDigit(value % 10) };
  d.setSegments(digits);
}

void clearDisplay(TM1637Display& d) {
  static const uint8_t BLANK[] = { 0, 0, 0, 0 };
  d.setSegments(BLANK);
}

void applyBrightness() {
  for (int i = 0; i < 3; i++) D[i]->setBrightness(brightness, true);
}

// --- Seriële helpers ---
void printTime(uint16_t secs) {
  uint16_t m = secs / 60, s = secs % 60;
  if (m < 10) Serial.print('0');
  Serial.print(m);
  Serial.print(':');
  if (s < 10) Serial.print('0');
  Serial.print(s);
}

void printTag(int i) {
  Serial.print('[');
  Serial.print(NAMES[i]);
  Serial.print(F("] "));
}

// --- EEPROM helpers ---
uint16_t readTimeFromEEPROM(int addr, uint16_t fallback) {
  uint16_t val;
  EEPROM.get(addr, val);
  if (val == 0 || val > 5999) return fallback;
  return val;
}

void saveTimeToEEPROM(int addr, uint16_t val) {
  EEPROM.put(addr, val);
}

uint8_t readByteFromEEPROM(int addr, uint8_t maxVal, uint8_t fallback) {
  uint8_t v = EEPROM.read(addr);
  return (v > maxVal) ? fallback : v;
}

// --- Start effects ---
void startFx(EffectState& fx) {
  fx.active = true;
  fx.step = 0;
  fx.lastChange = millis();
}
void startBeepEffect(int i)  { startFx(beepFx[i]); }
void startFlashEffect(int i) { startFx(flashFx[i]); }
void startSaveBeep(int i)    { startFx(saveFx[i]); }
void startCancelBeep(int i)  { startFx(cancelFx[i]); }
void startPreviewBeep()      { startFx(previewFx); }
void startClickFlash(int i)  { startFx(clickFx[i]); }

// =====================================================================
// Timer- en instelacties
// =====================================================================
void startTimer(int i, unsigned long now) {
  finished[i] = false;
  T[i].remain = T[i].setSecs;
  T[i].running = true;
  T[i].lastTick = now;
  printTag(i);
  Serial.print(F("Gestart: "));
  printTime(T[i].setSecs);
  Serial.println();
}

void enterSetting(int i) {
  mode[i] = DOUBLE_CLICK_MODE[i];
  finished[i] = false;
  settingActivity[i] = millis();
  printTag(i);
  if (mode[i] == MODE_BRIGHT) {
    settingOrig[i] = brightness;
    Serial.print(F("Helderheid-modus: "));
    Serial.print(brightness + 1);
    Serial.println(F("/8  (draaien = aanpassen, klik = bewaren)"));
  } else if (mode[i] == MODE_VOLUME) {
    settingOrig[i] = volumeLevel;
    Serial.print(F("Volume-modus: "));
    Serial.print(volumeLevel);
    Serial.print('/');
    Serial.print(MAX_VOLUME);
    Serial.println(F("  (draaien = aanpassen, klik = bewaren)"));
  }
}

void exitSetting(int i, bool save) {
  printTag(i);
  if (mode[i] == MODE_BRIGHT) {
    if (save) {
      EEPROM.update(ADDR_BRIGHT, brightness);
      Serial.print(F("Helderheid bewaard in EEPROM: "));
      Serial.println(brightness + 1);
    } else {
      brightness = settingOrig[i];
      applyBrightness();
      Serial.print(F("Helderheid-modus verlopen, niet bewaard. Terug naar: "));
      Serial.println(brightness + 1);
    }
  } else if (mode[i] == MODE_VOLUME) {
    if (save) {
      EEPROM.update(ADDR_VOLUME, volumeLevel);
      Serial.print(F("Volume bewaard in EEPROM: "));
      Serial.println(volumeLevel);
    } else {
      volumeLevel = settingOrig[i];
      Serial.print(F("Volume-modus verlopen, niet bewaard. Terug naar: "));
      Serial.println(volumeLevel);
    }
  }
  mode[i] = MODE_NORMAL;
  if (save) startSaveBeep(i);
}

void handleShortClick(int i, unsigned long now) {
  // In een instelmodus: klik = bewaren en verlaten
  if (mode[i] != MODE_NORMAL) {
    exitSetting(i, true);
    return;
  }

  // Lopende timer: stop & reset
  if (T[i].running) {
    T[i].running = false;
    T[i].remain = T[i].setSecs;
    startCancelBeep(i);
    printTag(i);
    Serial.println(F("Gestopt en gereset"));
    return;
  }

  // Encoders met dubbelklik-functie: wachten op mogelijke tweede klik
  if (DOUBLE_CLICK_MODE[i] != MODE_NORMAL) {
    if (T[i].clickPending && (now - T[i].clickAt) <= DOUBLE_CLICK_MS) {
      T[i].clickPending = false;
      enterSetting(i);
    } else {
      T[i].clickPending = true;
      T[i].clickAt = now;
    }
    return;
  }

  startTimer(i, now);
}

// =====================================================================
// Setup
// =====================================================================
void setup() {
  pinMode(swDev,  INPUT_PULLUP);
  pinMode(swStop, INPUT_PULLUP);
  pinMode(swFix,  INPUT_PULLUP);

  pinMode(buzzerPin, OUTPUT);
  buzPort = portOutputRegister(digitalPinToPort(buzzerPin));
  buzMask = digitalPinToBitMask(buzzerPin);
  buzzerOff();

  Serial.begin(9600);
  // Max 2 s wachten op de Serial Monitor, zodat de timer ook zonder PC opstart
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 2000) {
    delay(10);
  }

  Serial.println();
  Serial.println(F("=== Darkroom Multi-Timer ==="));
  Serial.print(F("Firmware Versie: "));
  Serial.println(firmwareVersion);

  // Helderheid & volume uit EEPROM
  brightness  = readByteFromEEPROM(ADDR_BRIGHT, MAX_BRIGHT, DEF_BRIGHT);
  volumeLevel = readByteFromEEPROM(ADDR_VOLUME, MAX_VOLUME, DEF_VOLUME);
  applyBrightness();

  // Tijden uit EEPROM of defaults
  T[0].setSecs = readTimeFromEEPROM(ADDR_DEV,  DEF_DEV);
  T[1].setSecs = readTimeFromEEPROM(ADDR_STOP, DEF_STOP);
  T[2].setSecs = readTimeFromEEPROM(ADDR_FIX,  DEF_FIX);

  for (int i = 0; i < 3; i++) {
    T[i].remain = T[i].setSecs;
    T[i].running = false;
    T[i].lastTick = millis();
    T[i].lastEncPos = 0;
    T[i].lastBtn = digitalRead(SW[i]);
    T[i].lastBtnChange = millis();
    T[i].pressStartMs = 0;
    T[i].clickPending = false;
    T[i].clickAt = 0;

    printTag(i);
    Serial.print(F("Ingestelde tijd: "));
    printTime(T[i].setSecs);
    Serial.println();
  }

  Serial.print(F("Helderheid: "));
  Serial.print(brightness + 1);
  Serial.println(F("/8"));
  Serial.print(F("Volume: "));
  Serial.print(volumeLevel);
  Serial.print('/');
  Serial.println(MAX_VOLUME);
  Serial.println(F("Dubbelklik DEV = helderheid, dubbelklik STOP = volume"));
  Serial.println(F("Klaar."));
}

// =====================================================================
// Main loop
// =====================================================================
void loop() {
  unsigned long now = millis();

  // --- Handle encoders ---
  for (int i = 0; i < 3; i++) {
    long raw = E[i]->read();
    long pos = raw / 4;
    long delta = pos - T[i].lastEncPos;
    if (delta == 0) continue;
    T[i].lastEncPos = pos;

    if (mode[i] == MODE_BRIGHT) {
      int v = (int)brightness + (int)delta;
      if (v < 0) v = 0;
      if (v > MAX_BRIGHT) v = MAX_BRIGHT;
      settingActivity[i] = now;
      if (v != brightness) {
        brightness = (uint8_t)v;
        applyBrightness();
        printTag(i);
        Serial.print(F("Helderheid: "));
        Serial.println(brightness + 1);
      }
    }
    else if (mode[i] == MODE_VOLUME) {
      int v = (int)volumeLevel + (int)delta;
      if (v < 0) v = 0;
      if (v > MAX_VOLUME) v = MAX_VOLUME;
      settingActivity[i] = now;
      if (v != volumeLevel) {
        volumeLevel = (uint8_t)v;
        startPreviewBeep();
        printTag(i);
        Serial.print(F("Volume: "));
        Serial.println(volumeLevel);
      }
    }
    else if (!T[i].running) {
      long newSecs = (long)T[i].setSecs + delta * ENC_STEP;
      if (newSecs < 0) newSecs = 0;
      if (newSecs > 5999) newSecs = 5999;
      if ((uint16_t)newSecs != T[i].setSecs) {
        T[i].setSecs = (uint16_t)newSecs;
        T[i].remain  = T[i].setSecs;
        finished[i] = false;
        printTag(i);
        Serial.print(F("Tijd ingesteld: "));
        printTime(T[i].setSecs);
        Serial.println();
      }
    }
  }

  // --- Handle buttons ---
  for (int i = 0; i < 3; i++) {
    bool lvl = digitalRead(SW[i]);
    if (lvl != T[i].lastBtn && (now - T[i].lastBtnChange) > 25) {
      T[i].lastBtn = lvl;
      T[i].lastBtnChange = now;
      if (lvl == LOW) {
        T[i].pressStartMs = now;
        startClickFlash(i);
      } else {
        // Losgelaten: korte klik als long-press niet al afgehandeld werd
        if (T[i].pressStartMs != 0 && (now - T[i].pressStartMs) < LONG_PRESS_MS) {
          handleShortClick(i, now);
        }
        T[i].pressStartMs = 0;
      }
    }

    // Long-press → save current setSecs to EEPROM (enkel in normale modus)
    if (T[i].pressStartMs && (lvl == LOW) && mode[i] == MODE_NORMAL && !T[i].running &&
        (now - T[i].pressStartMs >= LONG_PRESS_MS)) {
      T[i].clickPending = false;
      if (i == 0) saveTimeToEEPROM(ADDR_DEV,  T[i].setSecs);
      if (i == 1) saveTimeToEEPROM(ADDR_STOP, T[i].setSecs);
      if (i == 2) saveTimeToEEPROM(ADDR_FIX,  T[i].setSecs);
      startSaveBeep(i);
      T[i].pressStartMs = 0;
      printTag(i);
      Serial.print(F("Tijd bewaard in EEPROM: "));
      printTime(T[i].setSecs);
      Serial.println();
    }

    // Uitgestelde enkele klik (geen tweede klik gekomen) → timer starten
    if (T[i].clickPending && T[i].lastBtn == HIGH && (now - T[i].clickAt) > DOUBLE_CLICK_MS) {
      T[i].clickPending = false;
      startTimer(i, now);
    }

    // Instelmodus verlaten na inactiviteit (zonder bewaren)
    if (mode[i] != MODE_NORMAL && (now - settingActivity[i]) > SETTINGS_TIMEOUT_MS) {
      exitSetting(i, false);
    }
  }

  // --- Tick timers ---
  for (int i = 0; i < 3; i++) {
    if (T[i].running && now - T[i].lastTick >= 1000) {
      T[i].lastTick += 1000;
      if (T[i].remain > 0) T[i].remain--;
      if (T[i].remain == 0) {
        T[i].running = false;
        startBeepEffect(i);
        startFlashEffect(i);
        finished[i] = true;
        finishedAt[i] = millis();
        printTag(i);
        Serial.println(F("Klaar!"));
      }
    }
  }

  // --- Update beep effects (3 beeps) ---
  for (int i = 0; i < 3; i++) {
    if (beepFx[i].active && now - beepFx[i].lastChange >= 160) {
      beepFx[i].lastChange = now;
      if (beepFx[i].step % 2 == 0) buzzerTone(587);
      else buzzerOff();
      beepFx[i].step++;
      if (beepFx[i].step >= 6) {
        buzzerOff();
        beepFx[i].active = false;
      }
    }
  }

  // --- Update cancel/reset confirmation beep ---
  for (int i = 0; i < 3; i++) {
    if (cancelFx[i].active) {
      if (cancelFx[i].step == 0) {
        buzzerTone(330);
        cancelFx[i].lastChange = now;
        cancelFx[i].step = 1;
      } else if (cancelFx[i].step == 1 && now - cancelFx[i].lastChange >= 120) {
        buzzerOff();
        cancelFx[i].active = false;
      }
    }
  }

  // --- Update save confirmation beep ---
  for (int i = 0; i < 3; i++) {
    if (saveFx[i].active) {
      if (saveFx[i].step == 0) {
        buzzerTone(1480);
        saveFx[i].lastChange = now;
        saveFx[i].step = 1;
      } else if (saveFx[i].step == 1 && now - saveFx[i].lastChange >= 150) {
        buzzerOff();
        saveFx[i].active = false;
      }
    }
  }

  // --- Volume preview beep ---
  if (previewFx.active) {
    if (previewFx.step == 0) {
      buzzerTone(880);
      previewFx.lastChange = now;
      previewFx.step = 1;
    } else if (previewFx.step == 1 && now - previewFx.lastChange >= 90) {
      buzzerOff();
      previewFx.active = false;
    }
  }

  // --- Update flash effects (00:00 flashes) ---
  for (int i = 0; i < 3; i++) {
    if (flashFx[i].active && now - flashFx[i].lastChange >= 140) {
      flashFx[i].lastChange = now;
      flashFx[i].step++;
    }
    if (flashFx[i].active) {
      if (flashFx[i].step < 6) {
        if (flashFx[i].step % 2 == 0) D[i]->showNumberDecEx(0, 0x40, true);
        else clearDisplay(*D[i]);
      } else {
        flashFx[i].active = false;
      }
    }
  }

  // --- Update displays ---
  for (int i = 0; i < 3; i++) {
    // Klikbevestiging: display kort leeg
    if (clickFx[i].active) {
      if (now - clickFx[i].lastChange < CLICK_FLASH_MS) {
        clearDisplay(*D[i]);
        continue;
      }
      clickFx[i].active = false;
    }
    if (mode[i] == MODE_BRIGHT) {
      showSetting(*D[i], SEG_LOW_B, SEG_LOW_R, brightness + 1);   // "br 5"
      continue;
    }
    if (mode[i] == MODE_VOLUME) {
      showSetting(*D[i], SEG_LOW_U, SEG_LOW_O, volumeLevel);      // "uo 5"
      continue;
    }
    if (flashFx[i].active) continue;

    uint16_t toShow = T[i].running ? T[i].remain : T[i].setSecs;

    if (finished[i]) {
      bool colonBlink = ((now / 500) % 2);
      showTime(*D[i], T[i].setSecs, colonBlink);
      if (now - finishedAt[i] > 30000UL) finished[i] = false;
    }
    else if (T[i].running) {
      showTime(*D[i], toShow, (now / 1000) % 2);
    }
    else {
      showTime(*D[i], toShow, true);
    }
  }
}
