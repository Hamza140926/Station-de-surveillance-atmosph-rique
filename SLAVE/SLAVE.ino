// SLAVE: receive sensor values, show them on the LCD, and control LEDs/reset.
// The sensors are connected to the master, not to this Arduino.
// NaN means "no usable measurement", not a temperature of zero.
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <SPI.h>
#include <SdFat.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>
#include <math.h>
#include <string.h>
#include <StationRadio.h>

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
const unsigned long LINK_TIMEOUT_MS = StationRadio::LINK_TIMEOUT_MS;
const unsigned long PAGE_MS = 900;
const unsigned long ANIMATION_MS = 900;
const unsigned long BOOT_MS = 500;
const unsigned long SD_RETRY_MS = 5000;
const unsigned long SD_CHECK_MS = 1000;
const unsigned long SD_NOTICE_MS = 2000;
const unsigned long LINK_NOTICE_MS = 700;
const unsigned long LOG_MS = 1000;
const unsigned long DISPLAY_MS = 50;
StationRadio::Link radioLink(false);
bool radioWasReady = false;
bool linkNotice = false;
bool linkRestored = false;
bool radioLinkLost = false;
unsigned long linkNoticeStarted = 0;
uint32_t lastMeasurementSequence = 0;
uint32_t resetSequence = 0;
bool resetReplyPending = false;
unsigned long lastResetReply = 0;
bool measurementReplyPending = false;
uint32_t measurementReplySequence = 0;
unsigned long measurementReplyAt = 0;
unsigned long measurementReplyStarted = 0;
bool replyFailed = false;
unsigned long lastDisplay = 0;
unsigned long lastLog = 0;
bool storageAttempted = false;

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
    sdNoticeStarted = 0;
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

// Limit CSV writes to 1 Hz even though radio measurements arrive at 4 Hz.
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
    if (!storageAttempted || now - lastSdAttempt >= SD_RETRY_MS) {
      storageAttempted = true;
      bool mounted = startStorage();
      if (!mounted) {
        sdNotice = true;
        sdNoticeStarted = 0;
        Serial.println(F("SD unavailable - continuing without recording"));
      }
      lastSdAttempt = millis();
      lastSdCheck = lastSdAttempt;
    }
    updateBlueLed(millis());
    return;
  }
  if (pending && now - lastLog >= LOG_MS) {
    lastLog = now;
    logMeasurement();
  }
  updateBlueLed(millis());
}

void serviceRadio(unsigned long now) {
  radioLink.service(now);
  bool ready = radioLink.isReady();
  if (ready != radioWasReady) {
    Serial.println(ready ? F("NRF24 OK - waiting for master") :
                           F("NRF24 ERROR - check 3.3V/SPI; retrying"));
    radioWasReady = ready;
    if (!ready) radioLinkLost = true;
    linkNotice = false;
    refreshDisplay = true;
  }
}

bool sendRadioReply(StationRadio::MessageType type, uint32_t sequence) {
  bool delivered = radioLink.send(StationRadio::makePacket(type, sequence));
  if (!delivered && !replyFailed) Serial.println(F("NRF24 reply failed - no radio ACK"));
  else if (delivered && replyFailed) Serial.println(F("NRF24 reply recovered"));
  replyFailed = !delivered;
  return delivered;
}

// Allow the master to return to RX before transmitting an application reply.
void serviceReplies(unsigned long now) {
  if (resetReplyPending) {
    if (now - lastResetReply < 40) return;
    bool delivered = sendRadioReply(StationRadio::RESET_ACCEPTED, resetSequence);
    lastResetReply = millis();
    resetReplyPending = !delivered;
    return;
  }
  if (measurementReplyPending && now - measurementReplyStarted >= 100) {
    measurementReplyPending = false;
    Serial.println(F("NRF24 measurement reply timed out"));
  }
  if (measurementReplyPending && now - measurementReplyAt >= 5) {
    if (sendRadioReply(StationRadio::MEASUREMENT_ACCEPTED, measurementReplySequence)) {
      measurementReplyPending = false;
    }
    measurementReplyAt = millis();
  }
}

void receiveData(unsigned long now) {
  StationRadio::Packet packet;
  while (radioLink.receive(packet)) {
    now = millis();
    if (!StationRadio::validHeader(packet)) {
      if (resetState == RUNNING) {
        frameError = true;
        refreshDisplay = true;
      }
      Serial.println(F("NRF24 invalid packet header"));
      continue;
    }
    if (packet.type == StationRadio::RESET_REQUEST) {
      if (resetState == RUNNING) {
        resetSequence = packet.sequence;
        resetState = ANIMATING;
        resetStarted = now;
        sampleToLog = false;
        measurementReplyPending = false;
        moveFlag(CENTER);
        Serial.println(F("MASTER RESET REQUEST ACCEPTED"));
      }
      if (packet.sequence == resetSequence) {
        resetReplyPending = true;
        lastResetReply = now;
      }
    } else if (packet.type == StationRadio::MEASUREMENT && resetState == RUNNING) {
      if (!StationRadio::validMeasurement(packet)) {
        frameError = true;
        refreshDisplay = true;
        Serial.println(F("NRF24 invalid sensor values"));
        continue;
      }
      bool reconnecting = !haveData || radioLinkLost || now - lastData >= LINK_TIMEOUT_MS;
      if (haveData && !reconnecting && packet.sequence == lastMeasurementSequence) {
        measurementReplyPending = true;
        measurementReplySequence = packet.sequence;
        measurementReplyAt = now;
        measurementReplyStarted = now;
        continue;
      }
      if (reconnecting) {
        linkRestored = haveData;
        linkNotice = true;
        linkNoticeStarted = now;
        Serial.println(haveData ? F("MASTER LINK RESTORED") : F("MASTER CONNECTED"));
      }
      dsTemperature = packet.dsTemperature;
      bmpPressure = packet.pressure;
      haveData = true;
      radioLinkLost = false;
      frameError = false;
      lastData = now;
      lastMeasurementSequence = packet.sequence;
      sampleToLog = true;
      refreshDisplay = true;
      measurementReplyPending = true;
      measurementReplySequence = packet.sequence;
      measurementReplyAt = now;
      measurementReplyStarted = now;
    } else if (resetState == RUNNING) {
      frameError = true;
      refreshDisplay = true;
      Serial.println(F("NRF24 unexpected packet type"));
    }
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
  bool stale = !radioLink.isReady() || radioLinkLost ||
               !haveData || now - lastData >= LINK_TIMEOUT_MS;
  bool tempAlert = !stale && temperatureAlert();
  bool pressAlert = !stale && pressureAlert();
  bool haveTemperature = !isnan(dsTemperature);
  // Red: alarm, no usable temperature, bad message, or lost link.
  // Green: usable data with no alarm. Missing BMP alone does not force red.
  // Give the master time to start before calling the first missing message a fault.
  bool waitingAtBoot = radioLink.isReady() && !haveData &&
                       now - bootStarted < LINK_TIMEOUT_MS;
  bool alarm = !waitingAtBoot &&
               (stale || !haveTemperature || frameError || tempAlert || pressAlert);
  // Only fresh out-of-range measurements cause the continuous buzzer sound.
  // A radio error during an SD retry remains visible but is silent.
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
  if (now - lastDisplay < DISPLAY_MS) return;
  lastDisplay = now;
  if (!refreshDisplay && !stale) return;
  refreshDisplay = false;
  if (linkNotice && now - linkNoticeStarted >= LINK_NOTICE_MS) linkNotice = false;
  if (!radioLink.isReady()) {
    showFlashRow(0, F("ERREUR NRF24"));
    showFlashRow(1, F("VERIF 3V3 / SPI"));
    return;
  }
  if (stale) {
    showFlashRow(0, haveData ? F("LIAISON PERDUE") : F("RADIO OK"));
    showFlashRow(1, F("RECHERCHE MASTER"));
    return;
  }
  valueRow(0, PSTR("DS:"), dsTemperature, PSTR("C"));
  // Connection notices never mask a measured alarm or a sensor/frame warning.
  if (linkNotice && !tempAlert && !pressAlert && !frameError &&
      haveTemperature && !isnan(bmpPressure)) {
    showFlashRow(1, linkRestored ? F("LIEN RETABLI") : F("MASTER CONNECTE"));
    refreshDisplay = true;
    return;
  }
  if (linkNotice && (tempAlert || pressAlert || frameError)) {
    showFlashRow(1, frameError ? F("ERREUR TRAME") :
                   tempAlert ? F("ALERT TEMP") : F("ALERT PRESS"));
    refreshDisplay = true;
    return;
  }
  // Show the boot/storage warning briefly, without changing the alarm decision.
  if (sdNotice) {
    if (sdNoticeStarted == 0) sdNoticeStarted = now;
    if (now - sdNoticeStarted < SD_NOTICE_MS) {
      showFlashRow(1, sdWriteError ? F("ERREUR ECRIT SD") : F("SD ABSENTE / HS"));
      refreshDisplay = true;
      return;
    }
    sdNotice = false;
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
    unsigned long step = elapsed < 500 ? elapsed / 120 : (elapsed - 500) / 60;
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
    if (elapsed >= 400) {
      // Stop here and let the watchdog restart this Arduino in about 15 ms.
      wdt_enable(WDTO_15MS);
      while (true) {}
    }
  }
}

// Run once at power-on or after reset.
// Deselect both SPI devices before starting storage or the radio.
void setup() {
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);
  digitalWrite(BLUE_LED, LOW);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  pinMode(StationRadio::CSN_PIN, OUTPUT);
  digitalWrite(StationRadio::CSN_PIN, HIGH);
  pinMode(StationRadio::CE_PIN, OUTPUT);
  digitalWrite(StationRadio::CE_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, LOW);
  flagServo.attach(SERVO_PIN);
  moveFlag(CENTER);
  Serial.begin(115200);
  Wire.begin();
  Wire.setWireTimeout(3000, true);
  lcd.init();
  lcd.backlight();
  showFlashRow(0, F("INIT NRF24..."));
  showFlashRow(1, F("PATIENTER..."));
  // Start listening before attempting potentially slow SD initialization.
  lastSdAttempt = millis();
  lastSdCheck = lastSdAttempt;
  radioWasReady = radioLink.begin(millis());
  Serial.println(radioWasReady ? F("NRF24 OK - waiting for master") :
                                F("NRF24 ERROR - check 3.3V/SPI; retrying"));
  showFlashRow(0, radioWasReady ? F("RADIO OK") : F("ERREUR NRF24"));
  showFlashRow(1, radioWasReady ? F("RECHERCHE MASTER") : F("VERIF 3V3 / SPI"));
  lastPage = millis();
  bootStarted = millis();
  sdNotice = false;
  sdNoticeStarted = 0;
  updateBlueLed(bootStarted);
  updateSound(bootStarted);
}

// Receive data/reset requests, then update LCD, LEDs, flag and sound.
void loop() {
  unsigned long now = millis();
  serviceRadio(now);
  receiveData(now);
  serviceReplies(millis());
  now = millis();
  if (resetState == RUNNING) {
    updateNormalDisplay(now);
    updateSound(now);
    // Give initial connection and application replies priority over SD setup.
    if (now - bootStarted >= BOOT_MS &&
        !measurementReplyPending && !resetReplyPending &&
        (haveData || now - bootStarted >= LINK_TIMEOUT_MS)) serviceStorage(now);
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
