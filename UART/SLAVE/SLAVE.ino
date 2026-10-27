// SLAVE: receive sensor values, show them on the LCD, and control LEDs/reset.
// The sensors are connected to the master, not to this Arduino.
// NaN means "no usable measurement", not a temperature of zero.
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <SPI.h>
#include <SdFat.h>114
µ
#1include <avr/wdt.h>
#include <avr/pgmspace.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Run before setup(). Turn off the watchdog left active by a software reset.
// The watchdog is a timer that can restart the Arduino.
void disableWatchdogAtBoot() __attribute__((naked, section(".init3")));
void disableWatchdogAtBoot() {
  MCUSR = 0;
  wdt_disable();
}

// LCD address and size: 16 characters per row, two rows.
LiquidCrystal_I2C lcd(0x27, 16, 2);
const uint8_t RED_LED = 6;
const uint8_t GREEN_LED = 5;
const uint8_t BUZZER_PIN = 2;
const uint8_t SERVO_PIN = 3;
const uint8_t BLUE_LED = 8;
const uint8_t SD_CS_PIN = 10;
const int CENTER = 90;
const int RED_POS = 180;
const int GREEN_POS = 0;
Servo flagServo;
// Alarm limits. Exactly 30 C is an alert; exactly 950 or 1050 hPa is normal.
const float TEMP_LIMIT = 30.0f;
const float PRESSURE_MIN = 950.0f;
const float PRESSURE_MAX = 1050.0f;
// Times are in milliseconds: 1000 ms = 1 second.
const unsigned long LINK_TIMEOUT_MS = 3500;
const unsigned long PAGE_MS = 1800;
const unsigned long ANIMATION_MS = 1800;
const unsigned long BOOT_MS = 900;
const unsigned long SD_RETRY_MS = 5000;
const unsigned long SD_CHECK_MS = 1000;
const unsigned long SD_NOTICE_MS = 2000;

// SdFat32 uses FAT16/FAT32 cards, not exFAT.
// Its sync() result lets us check that saving really succeeded.
SdFat32 sd;
cid_t mountedCardId;
bool sdReady = false;
bool sdNotice = false;
bool sdWriteError = false;
bool sampleToLog = false;
char logFilename[13] = {0};
unsigned long lastSdAttempt = 0, lastSdCheck = 0, sdNoticeStarted = 0;

// Latest values from a checked message. Start with no measurements.
float dsTemperature = NAN, bmpPressure = NAN;
bool haveData = false, frameError = false;
unsigned long lastData = 0, lastPage = 0;
uint8_t page = 0;
bool refreshDisplay = true;
// Remember the displayed text to avoid rewriting unchanged rows.
// Each row needs 16 characters plus a final zero marking the end of the text.
char previousRows[2][17] = {{0}, {0}};

// The master starts reset; the slave animates, then restarts itself.
enum ResetState { RUNNING, ANIMATING, RESET_LOCAL };
ResetState resetState = RUNNING;
unsigned long resetStarted = 0;
unsigned long bootStarted = 0;
bool alarmActive = false;
int lastServoPosition = -1;
unsigned int lastSound = 0;

// Move only when the wanted position changes. Center during boot and reset.
void moveFlag(int position) {
  if (position == lastServoPosition) return;
  flagServo.write(position);
  lastServoPosition = position;
}

// Change the passive buzzer's note only when needed. Zero means silence.
void playNote(unsigned int frequency) {
  if (frequency == lastSound) return;
  if (frequency == 0) noTone(BUZZER_PIN);
  else tone(BUZZER_PIN, frequency);
  lastSound = frequency;
}

// Play a rising boot tune, a falling reset tune, or a steady alarm.
// No delay is used, so messages and the LCD keep working during sounds.
void updateSound(unsigned long now) {
  if (resetState != RUNNING) {
    unsigned long elapsed = now - resetStarted;
    if (resetState == ANIMATING && elapsed < 600) {
      playNote(elapsed < 200 ? 1047 : elapsed < 400 ? 784 : 523);
    } else playNote(0);
  } else if (now - bootStarted < BOOT_MS) {
    unsigned long elapsed = now - bootStarted;
    playNote(elapsed < 150 ? 523 : elapsed < 300 ? 659 : elapsed < 450 ? 784 : 0);
  } else playNote(alarmActive ? 1600 : 0);
}

// Give the LED and flag the same normal/alert decision.
// Sound is chosen separately: communication/storage faults must not cause beeps.
// The startup period keeps the flag centered before normal control begins.
void setIndicators(bool alarm, unsigned long now) {
  if (now - bootStarted < BOOT_MS) {
    digitalWrite(RED_LED, LOW);
    digitalWrite(GREEN_LED, LOW);
    moveFlag(CENTER);
    return;
  }
  digitalWrite(RED_LED, alarm ? HIGH : LOW);
  digitalWrite(GREEN_LED, alarm ? LOW : HIGH);
  moveFlag(alarm ? RED_POS : GREEN_POS);
}

// Make the same check number (CRC) as the master.
// If it differs from the received number, reject the damaged message.
uint16_t crc16(const char *text) {
  uint16_t crc = 0xFFFF;
  while (*text) {
    crc ^= uint16_t(uint8_t(*text++)) << 8;
    for (uint8_t i = 0; i < 8; ++i)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

// Show text on LCD row 0 (top) or row 1 (bottom).
// Fill unused positions with spaces to erase old letters.
// Skip the write if the row already shows the same text.
void showRow(uint8_t row, const char *text) {
  char padded[17];
  memset(padded, ' ', 16);
  padded[16] = '\0';
  size_t length = strlen(text);
  if (length > 16) length = 16;
  memcpy(padded, text, length);
  if (strcmp(previousRows[row], padded) == 0) return;
  lcd.setCursor(0, row);
  lcd.print(padded);
  strcpy(previousRows[row], padded);
}

// Read a fixed message from program memory, using only a small temporary row.
// F("text") keeps fixed messages out of the Uno's limited RAM.
void showFlashRow(uint8_t row, const __FlashStringHelper *text) {
  char line[17];
  strncpy_P(line, reinterpret_cast<PGM_P>(text), sizeof(line) - 1);
  line[16] = '\0';
  showRow(row, line);
}

// Build a measurement row, for example "DS:25.5 C" or "BMPR:NaN hPa".
// Display numbers with one decimal place, then send the text to showRow().
// PSTR labels and units stay in program memory; %S reads them from there.
void valueRow(uint8_t row, PGM_P label, float value, PGM_P unit) {
  char number[9], line[17];
  if (isnan(value)) strcpy_P(number, PSTR("NaN"));
  else dtostrf(value, 0, 1, number);
  snprintf_P(line, sizeof(line), PSTR("%S%s %S"), label, number, unit);
  showRow(row, line);
}

// Blink blue when recording is unavailable; keep blue ON when the card is ready.
// This LED is separate from the red/green temperature and pressure indicators.
void updateBlueLed(unsigned long now) {
  digitalWrite(BLUE_LED, sdReady || (now / 300) % 2 ? HIGH : LOW);
}

// Stop using a failed card, and show a short warning once on this transition.
// Do not stop sensor readings, the flag, or temperature/pressure alarms.
void storageFailed(unsigned long now, bool writeError) {
  if (sdReady) {
    sdNotice = true;
    sdNoticeStarted = now;
  }
  sdReady = false;
  sdWriteError = writeError;
  sd.end();
  lastSdAttempt = millis();
  refreshDisplay = true;
  updateBlueLed(millis());
}

// Append one CSV row, or create a new file/header when text is null.
// Check writing, saving and closing. Never overwrite an existing session.
bool appendRecord(const char *text) {
  FatFile file;
  uint8_t flags = text ? O_WRONLY | O_APPEND : O_WRONLY | O_CREAT | O_EXCL;
  if (!file.open(&sd, logFilename, flags)) return false;
  // A null text asks for a session header, read from program memory one byte at a time.
  bool written;
  if (text) {
    size_t length = strlen(text);
    written = file.write(text, length) == length;
  } else {
    static const char header[] PROGMEM =
        "sep=;\r\nTemps_s;Temperature_DS_C;Pression_BMP_hPa;"
        "Alerte_temperature;Alerte_pression\r\n";
    written = true;
    for (size_t i = 0; i < sizeof(header) - 1; ++i) {
      uint8_t value = pgm_read_byte(header + i);
      if (file.write(&value, 1) != 1) {
        written = false;
        break;
      }
    }
  }
  bool saved = file.sync();
  bool closed = file.close();
  return written && saved && closed;
}

// Pick a new session filename so Excel sees one clean table per session.
// LOG00001.CSV, LOG00002.CSV, etc. Existing recordings are left untouched.
bool createLogFile() {
  for (unsigned long number = 1; number <= 99999; ++number) {
    snprintf_P(logFilename, sizeof(logFilename), PSTR("LOG%05lu.CSV"), number);
    if (!sd.exists(logFilename)) return appendRecord(NULL);
  }
  return false;
}

// Try the SPI module and card. A missing card or bad wiring makes this fail.
// Success also requires a writable FAT16/FAT32 card and a saved session header.
bool startStorage() {
  sd.end();
  if (!sd.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4)))) {
    sdWriteError = false;
    sd.end();
    return false;
  }
  if (!sd.card()->readCID(&mountedCardId) ||
      !createLogFile()) {
    sdWriteError = true;
    sd.end();
    return false;
  }
  sdReady = true;
  sdWriteError = false;
  return true;
}

// Format a measurement for French Excel, using a decimal comma.
// Keep six decimals so a value close to an alarm limit is not hidden by rounding.
void csvValue(float value, char *text) {
  if (isnan(value)) strcpy_P(text, PSTR("NaN"));
  else dtostrf(value, 0, 6, text);
  for (char *cursor = text; *cursor; ++cursor)
    if (*cursor == '.') *cursor = ',';
}

// Save receive time in seconds, DS temperature, pressure and two alert columns.
// Missing readings are NaN / INDISPONIBLE, never NORMAL.
void logMeasurement() {
  char dsText[12], pressureText[12], line[72];
  csvValue(dsTemperature, dsText);
  csvValue(bmpPressure, pressureText);
  PGM_P tempStatus = isnan(dsTemperature) ? PSTR("INDISPONIBLE") :
                    temperatureAlert() ? PSTR("ALERTE") : PSTR("NORMAL");
  PGM_P pressStatus = isnan(bmpPressure) ? PSTR("INDISPONIBLE") :
                     pressureAlert() ? PSTR("ALERTE") : PSTR("NORMAL");
  int length = snprintf_P(line, sizeof(line), PSTR("%lu,%03u;%s;%s;%S;%S\r\n"),
                         lastData / 1000UL, unsigned(lastData % 1000UL),
                         dsText, pressureText, tempStatus, pressStatus);
  if (length < 0 || size_t(length) >= sizeof(line)) {
    storageFailed(millis(), true);
    return;
  }
  if (!appendRecord(line)) storageFailed(millis(), true);
}

// Check a working card once a second, and retry an unavailable card every 5 s.
// There is no card-detect wire: an actual SPI reply checks presence/identity.
// Samples received without a ready card are not saved for later.
void serviceStorage(unsigned long now) {
  bool pending = sampleToLog;
  sampleToLog = false;
  if (sdReady && now - lastSdCheck >= SD_CHECK_MS) {
    cid_t currentId;
    lastSdCheck = now;
    if (!sd.card()->readCID(&currentId) ||
        memcmp(&currentId, &mountedCardId, sizeof(currentId)) != 0) {
      storageFailed(millis(), false);
    }
  }
  now = millis();
  if (!sdReady) {
    if (now - lastSdAttempt >= SD_RETRY_MS) {
      startStorage();
      lastSdAttempt = millis();
      lastSdCheck = lastSdAttempt;
    }
    updateBlueLed(millis());
    return;
  }
  if (pending) logMeasurement();
  updateBlueLed(millis());
}

// Turn one received text field into a number within the allowed range.
// "NaN" is allowed and means that this sensor reading is unavailable.
// Return false for invalid text; on success, put the result into value.
bool parseValue(const char *text, float minimum, float maximum, float &value) {
  if (strcmp_P(text, PSTR("NaN")) == 0) {
    value = NAN;
    return true;
  }
  // Allow an optional minus sign, digits, and one decimal point.
  const char *cursor = text;
  if (*cursor == '-') ++cursor;
  bool digit = false, dot = false;
  for (; *cursor; ++cursor) {
    if (*cursor >= '0' && *cursor <= '9') digit = true;
    else if (*cursor == '.' && !dot) dot = true;
    else return false;
  }
  if (!digit) return false;
  char *end;
  value = strtod(text, &end);
  return *end == '\0' && isfinite(value) && value >= minimum && value <= maximum;
}

// Check one whole measurement line: its CRC, commas, and three values.
// Split the text into fields by replacing separators with end-of-text zeros.
// Update the saved measurements only if every check passes.
bool parsePacket(char *line) {
  char *star = strchr(line, '*');
  if (!star || strlen(star + 1) != 4) return false;
  for (const char *ch = star + 1; *ch; ++ch)
    if (!((*ch >= '0' && *ch <= '9') || (*ch >= 'A' && *ch <= 'F'))) return false;
  uint16_t checksum = strtoul(star + 1, NULL, 16);
  *star = '\0';
  if (crc16(line) != checksum || strncmp_P(line, PSTR("D,"), 2) != 0) return false;

  char *ds = line + 2;
  char *bm = strchr(ds, ',');
  if (!bm) return false;
  *bm++ = '\0';
  char *pressure = strchr(bm, ',');
  if (!pressure) return false;
  *pressure++ = '\0';
  if (strchr(pressure, ',')) return false;

  // Check temporary values first so a bad message cannot partly update the data.
  // BMP temperature and pressure must either both be numbers or both be NaN.
  float newDs, newBm, newPressure;
  if (!parseValue(ds, -55.0f, 125.0f, newDs) ||
      !parseValue(bm, -40.0f, 85.0f, newBm) ||
      !parseValue(pressure, 300.0f, 1100.0f, newPressure) ||
      isnan(newBm) != isnan(newPressure)) return false;
  dsTemperature = newDs;
  bmpPressure = newPressure;
  return true;
}

// Collect incoming serial characters until a complete line arrives.
// "R" means the master requests reset. Other lines are checked as measurements
// during normal operation. Reject lines that are too long or contain bad data.
void receiveData(unsigned long now) {
  static char line[64];
  static uint8_t length = 0;
  static bool overflow = false;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      line[length] = '\0';
      if (!overflow && strcmp_P(line, PSTR("R")) == 0) {
        if (resetState == RUNNING) {
          resetState = ANIMATING;
          resetStarted = now;
          moveFlag(CENTER);
        }
        // Repeated requests get a reply but do not restart the animation.
        Serial.println(F("A"));
      } else if (resetState == RUNNING) {
        if (!overflow && parsePacket(line)) {
          haveData = true;
          frameError = false;
          lastData = now;
          sampleToLog = true;
        } else {
          frameError = true;
        }
        refreshDisplay = true;
      }
      length = 0;
      overflow = false;
    } else if (ch == '\0') overflow = true;
    else if (length < sizeof(line) - 1) line[length++] = ch;
    else overflow = true;
  }
}

// Only DS1621 decides the temperature alert. BMP is used for pressure only.
bool temperatureAlert() {
  return !isnan(dsTemperature) && dsTemperature >= TEMP_LIMIT;
}

// Return true if available BMP pressure is below 950 or above 1050 hPa.
// Missing pressure alone is not a pressure alert.
bool pressureAlert() {
  return !isnan(bmpPressure) &&
         (bmpPressure < PRESSURE_MIN || bmpPressure > PRESSURE_MAX);
}

// During normal operation, choose the LED colour and update the LCD pages.
// With BMP missing, use DS temperature and keep showing a CHECK BMP280 warning.
// If no valid message arrives for 3.5 seconds, stop displaying old measurements.
void updateNormalDisplay(unsigned long now) {
  bool stale = !haveData || now - lastData >= LINK_TIMEOUT_MS;
  bool tempAlert = !stale && temperatureAlert();
  bool pressAlert = !stale && pressureAlert();
  bool haveTemperature = !isnan(dsTemperature);
  // Red: alarm, no usable temperature, bad message, or lost link.
  // Green: usable data with no alarm. Missing BMP alone does not force red.
  // Give the master time to start before calling the first missing message a fault.
  bool waitingAtBoot = !haveData && now - bootStarted < LINK_TIMEOUT_MS;
  bool alarm = !waitingAtBoot &&
               (stale || !haveTemperature || frameError || tempAlert || pressAlert);
  // Only fresh out-of-range measurements cause the continuous buzzer sound.
  // A serial error during an SD retry remains visible but is silent.
  alarmActive = tempAlert || pressAlert;
  if (waitingAtBoot) {
    alarmActive = false;
    digitalWrite(RED_LED, LOW);
    digitalWrite(GREEN_LED, LOW);
    moveFlag(CENTER);
  } else setIndicators(alarm, now);

  if (now - lastPage >= PAGE_MS) {
    // Three pages: pressure value, temperature status, pressure/sensor status.
    page = (page + 1) % 3;
    lastPage = now;
    refreshDisplay = true;
  }
  if (!refreshDisplay && !stale) return;
  refreshDisplay = false;
  // DS stays on the top row; the bottom row changes with the page.
  valueRow(0, PSTR("DS:"), stale ? NAN : dsTemperature, PSTR("C"));
  // Show the boot/storage warning briefly, without changing the alarm decision.
  if (sdNotice) {
    if (now - sdNoticeStarted < SD_NOTICE_MS) {
      showFlashRow(1, sdWriteError ? F("ERREUR ECRIT SD") : F("SD ABSENTE / HS"));
      refreshDisplay = true;
      return;
    }
    sdNotice = false;
  }
  if (stale) {
    showFlashRow(1, haveData ? F("LIAISON PERDUE") : F("ATTENTE MASTER"));
    return;
  }
  switch (page) {
    case 0: valueRow(1, PSTR("BMPR:"), bmpPressure, PSTR("hPa")); break;
    case 1:
      showFlashRow(1, tempAlert ? F("ALERT TEMP") :
                   !haveTemperature ? F("CHECK DS1621") : F("NORMALE TEMP"));
      break;
    case 2:
      showFlashRow(1, frameError ? F("ERREUR TRAME") :
                   pressAlert ? F("ALERT PRESS") :
                   isnan(bmpPressure) ? F("CHECK BMP280") :
                   isnan(dsTemperature) ? F("CHECK DS1621") : F("PRESSION NORMALE"));
      break;
  }
}

// Advance the reset sequence without a long blocking delay.
// Keep the flag centered, animate LEDs, then restart this slave.
// The master waits for this sequence before restarting itself.
void updateReset(unsigned long now) {
  unsigned long elapsed = now - resetStarted;
  moveFlag(CENTER);
  showFlashRow(0, F("RESET SYSTEME"));
  if (resetState == ANIMATING) {
    // Take turns lighting red and green, faster towards the end.
    unsigned long step = elapsed < 1000 ? elapsed / 180 : (elapsed - 1000) / 80;
    digitalWrite(RED_LED, (step % 2) ? HIGH : LOW);
    digitalWrite(GREEN_LED, (step % 2) ? LOW : HIGH);
    char progress[17];
    memset(progress, ' ', 16);
    progress[16] = '\0';
    // Fill the LCD progress bar with more '=' characters as time passes.
    uint8_t filled = elapsed >= ANIMATION_MS ? 16 : elapsed * 16 / ANIMATION_MS;
    memset(progress, '=', filled);
    showRow(1, progress);
    if (elapsed >= ANIMATION_MS) {
      resetState = RESET_LOCAL;
      resetStarted = now;
    }
  } else if (resetState == RESET_LOCAL) {
    showFlashRow(1, F("REDEMARRAGE..."));
    if (elapsed >= 800) {
      // Stop here and let the watchdog restart this Arduino in about 15 ms.
      wdt_enable(WDTO_15MS);
      while (true) {}
    }
  }
}

// Run once at power-on or after reset.
// Prepare LEDs, buzzer, centered flag, serial link and LCD.
void setup() {
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);
  digitalWrite(BLUE_LED, LOW);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, LOW);
  flagServo.attach(SERVO_PIN);
  moveFlag(CENTER);
  // The first card check may take time. Start receiving only after it finishes.
  // Later messages are complete lines, so reception can recover at a newline.
  startStorage();
  lastSdAttempt = millis();
  lastSdCheck = lastSdAttempt;
  Serial.begin(9600);
  Wire.begin();
  Wire.setWireTimeout(3000, true);
  lcd.init();
  lcd.backlight();
  showFlashRow(0, F("SYSTEME PRET"));
  showFlashRow(1, F("ATTENTE MASTER"));
  lastPage = millis();
  bootStarted = millis();
  sdNotice = !sdReady;
  sdNoticeStarted = bootStarted;
  updateBlueLed(bootStarted);
  updateSound(bootStarted);
}

// Receive data/reset requests, then update LCD, LEDs, flag and sound.
void loop() {
  unsigned long now = millis();
  receiveData(now);
  if (resetState == RUNNING) {
    updateNormalDisplay(now);
    updateSound(now);
    // Do not attempt a slow card initialization during the short boot melody.
    if (now - bootStarted >= BOOT_MS) serviceStorage(now);
  } else {
    sampleToLog = false;
    updateReset(now);
  }
  updateBlueLed(millis());
  if (Wire.getWireTimeoutFlag()) {
    Wire.clearWireTimeoutFlag();
    // A slow/blocked LCD bus caused a timeout. Forget the saved row text so
    // the next update tries writing again, and light red to signal the fault.
    previousRows[0][0] = previousRows[1][0] = '\0';
    refreshDisplay = true;
    if (resetState == RUNNING) setIndicators(true, now);
  }
  updateSound(millis());
}
