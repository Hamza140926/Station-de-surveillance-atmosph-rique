// MASTER: read the two sensors and send their values to the slave.
// I2C: A4/A5. nRF24L01: CE D8, CSN D10, SPI D11/D12/D13.
// NaN means "no usable measurement", not a temperature of zero.
#include <Wire.h>
#include <avr/wdt.h>
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

// Sensor address and waiting times. MS means milliseconds; US means microseconds.
const uint8_t DS_ADDRESS = 0x48;
const uint8_t RESET_BUTTON = 2;  // Button between master D2 and GND.
const unsigned long RESET_ACK_MS = 1500;
const unsigned long RESET_FINISH_MS = 1600;
const unsigned long SAMPLE_MS = 250;
const unsigned long RETRY_MS = 3000;
const unsigned long BUS_RELEASE_US = 3000;

// Factory correction numbers stored inside the BMP280.
// They turn the sensor's raw numbers into temperature and pressure.
struct Calibration {
  uint16_t t1;
  int16_t t2, t3;
  uint16_t p1;
  int16_t p2, p3, p4, p5, p6, p7, p8, p9;
};

Calibration calibration;
uint8_t bmpAddress = 0;  // Zero means no BMP280 is currently ready.
bool dsReady = false;
bool dsConfigPending = false;
bool dsReadFailed = false;
// Save times instead of using long delays, so the main loop can keep running.
unsigned long dsStartedAt = 0;
unsigned long lastDsRetry = 0;
unsigned long lastBmpRetry = 0;
unsigned long lastSample = 0;
bool resetPending = false;
bool resetAcknowledged = false;
unsigned long resetAt = 0;
unsigned long lastResetRequest = 0;
unsigned long resetAcceptedAt = 0;
StationRadio::Link radioLink(true);
uint32_t packetSequence = 0;
uint32_t lastMeasurementSequence = 0;
uint32_t resetSequence = 0;
bool measurementSent = false;
bool linkConfirmed = false;
bool radioWasReady = false;
bool transmitFailed = false;
unsigned long lastRadioAck = 0;

void serviceRadio(unsigned long now) {
  radioLink.service(now);
  bool ready = radioLink.isReady();
  if (ready != radioWasReady) {
    Serial.println(ready ? F("NRF24 OK - waiting for slave") :
                           F("NRF24 ERROR - check 3.3V/SPI; retrying"));
    radioWasReady = ready;
  }
  if (linkConfirmed && (!ready || now - lastRadioAck >= StationRadio::LINK_TIMEOUT_MS)) {
    linkConfirmed = false;
    Serial.println(F("SLAVE LINK LOST - waiting for valid reply"));
  }
}

bool sendRadioPacket(const StationRadio::Packet &packet) {
  bool delivered = radioLink.send(packet);
  if (!delivered && !transmitFailed)
    Serial.println(F("NRF24 TX FAILED - no radio ACK"));
  else if (delivered && transmitFailed)
    Serial.println(F("NRF24 TX recovered"));
  transmitFailed = !delivered;
  return delivered;
}

// Return true if the last I2C operation did not time out.
// A timeout means the sensor or bus took too long to answer.
bool busOK() {
  if (!Wire.getWireTimeoutFlag()) return true;
  Wire.clearWireTimeoutFlag();
  return false;
}

// Wait briefly for the I2C clock wire to be released.
// Return false after 3 ms rather than waiting forever.
bool waitForClockHigh() {
  unsigned long started = micros();
  while (digitalRead(SCL) == LOW) {
    if (micros() - started >= BUS_RELEASE_US) return false;
  }
  return true;
}

// Try to free the shared sensor bus after a failed read.
// Send up to nine clock pulses, then a STOP signal, and restart Wire.
// Return false if a wire stays LOW. This cannot repair a short circuit.
bool recoverI2CBus() {
  Wire.end();
  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
  bool released = waitForClockHigh();

  // A sensor may be waiting for the rest of an interrupted message.
  // Only pull wires LOW or release them; do not force them HIGH.
  for (uint8_t pulse = 0; released && digitalRead(SDA) == LOW && pulse < 9; ++pulse) {
    digitalWrite(SCL, LOW);
    pinMode(SCL, OUTPUT);
    delayMicroseconds(5);
    pinMode(SCL, INPUT_PULLUP);
    released = waitForClockHigh();
    delayMicroseconds(5);
  }

  if (released) {
    // Release SDA while SCL is HIGH: this is the I2C STOP signal.
    digitalWrite(SCL, LOW);
    pinMode(SCL, OUTPUT);
    digitalWrite(SDA, LOW);
    pinMode(SDA, OUTPUT);
    delayMicroseconds(5);
    pinMode(SCL, INPUT_PULLUP);
    released = waitForClockHigh();
    delayMicroseconds(5);
    pinMode(SDA, INPUT_PULLUP);
    delayMicroseconds(5);
    released = released && digitalRead(SDA) == HIGH;
  }

  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
  Wire.begin();
  Wire.setWireTimeout(3000, true);
  Wire.clearWireTimeoutFlag();
  return released;
}

// Put one setting into a sensor.
// address selects the sensor; reg selects its internal setting; value is the setting.
// Return true only if the sensor accepted the message without a timeout.
bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.clearWireTimeoutFlag();
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  uint8_t result = Wire.endTransmission();
  bool healthy = busOK();
  return result == 0 && healthy;
}

// Send a one-byte instruction, such as "start measuring", to a sensor.
// Return true if the sensor accepted it without a timeout.
bool sendCommand(uint8_t address, uint8_t command) {
  Wire.clearWireTimeoutFlag();
  Wire.beginTransmission(address);
  Wire.write(command);
  uint8_t result = Wire.endTransmission();
  bool healthy = busOK();
  return result == 0 && healthy;
}

// Read count bytes from a sensor, starting at its internal location reg.
// Store the bytes in data. Return false if any bytes are missing or the bus fails.
bool readRegisters(uint8_t address, uint8_t reg, uint8_t *data, uint8_t count) {
  Wire.clearWireTimeoutFlag();
  Wire.beginTransmission(address);
  Wire.write(reg);
  uint8_t result = Wire.endTransmission();
  bool healthy = busOK();
  if (result != 0 || !healthy) return false;
  uint8_t received = Wire.requestFrom(address, count);
  healthy = busOK();
  if (!healthy || received != count || Wire.available() < count) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < count; ++i) data[i] = Wire.read();
  return true;
}

// Begin setting up the DS1621 for repeated temperature measurements.
// now is the current time from millis(). The next step happens in serviceDS().
void startDS(unsigned long now) {
  lastDsRetry = now;
  dsReady = false;
  dsReadFailed = false;
  dsConfigPending = writeRegister(DS_ADDRESS, 0xAC, 0x00);
  dsStartedAt = now;
}

// Finish DS1621 setup without a long delay, or retry after a lasting failure.
// Calling this often does not mean the sensor is restarted every time.
void serviceDS(unsigned long now) {
  // Give the sensor 20 ms to save its setting before asking it to measure.
  if (dsConfigPending && now - dsStartedAt >= 20) {
    dsConfigPending = false;
    dsReady = sendCommand(DS_ADDRESS, 0xEE);
    dsStartedAt = now;
    if (!dsReady) lastDsRetry = now;
  } else if ((!dsReady || dsReadFailed) && !dsConfigPending &&
             now - lastDsRetry >= RETRY_MS) {
    startDS(now);
  }
}

// Get a fresh DS1621 temperature in degrees C.
// After a failed read, try bus recovery and one more read.
// Return NaN if the sensor is not ready or no usable reading is available.
float readDS(unsigned long now) {
  serviceDS(now);
  if (!dsReady || now - dsStartedAt < 800) return NAN;
  uint8_t bytes[2];
  // Recover the shared bus before retrying, without restarting DS conversion.
  if (!readRegisters(DS_ADDRESS, 0xAA, bytes, sizeof(bytes)) &&
      (!recoverI2CBus() || !readRegisters(DS_ADDRESS, 0xAA, bytes, sizeof(bytes)))) {
    if (!dsReadFailed) lastDsRetry = now;
    dsReadFailed = true;
    return NAN;
  }
  // The first byte gives whole degrees, including negative values.
  // The top bit of the second byte adds half a degree.
  float value = int8_t(bytes[0]) + ((bytes[1] & 0x80) ? 0.5f : 0.0f);
  if (value < -55.0f || value > 125.0f) {
    if (!dsReadFailed) lastDsRetry = now;
    dsReadFailed = true;
    return NAN;
  }
  dsReadFailed = false;
  return value;
}

// Join two bytes into one number. The sensor sends the lower part first.
uint16_t littleEndian(const uint8_t *bytes) {
  return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
}

// Check that a BMP280 is present at this address and prepare it to measure.
// Read its factory correction numbers twice to check that the reads agree.
// Return false if a check or setting fails.
bool initializeBMP(uint8_t address) {
  uint8_t id, status;
  if (!readRegisters(address, 0xD0, &id, 1) || id != 0x58 ||
      !readRegisters(address, 0xF3, &status, 1) || (status & 0x01)) return false;

  uint8_t first[24], second[24];
  if (!readRegisters(address, 0x88, first, sizeof(first)) ||
      !readRegisters(address, 0x88, second, sizeof(second)) ||
      memcmp(first, second, sizeof(first)) != 0) return false;

  calibration.t1 = littleEndian(first);
  calibration.t2 = int16_t(littleEndian(first + 2));
  calibration.t3 = int16_t(littleEndian(first + 4));
  calibration.p1 = littleEndian(first + 6);
  calibration.p2 = int16_t(littleEndian(first + 8));
  calibration.p3 = int16_t(littleEndian(first + 10));
  calibration.p4 = int16_t(littleEndian(first + 12));
  calibration.p5 = int16_t(littleEndian(first + 14));
  calibration.p6 = int16_t(littleEndian(first + 16));
  calibration.p7 = int16_t(littleEndian(first + 18));
  calibration.p8 = int16_t(littleEndian(first + 20));
  calibration.p9 = int16_t(littleEndian(first + 22));
  if (calibration.t1 == 0 || calibration.t1 == 0xFFFF ||
      calibration.p1 == 0 || calibration.p1 == 0xFFFF) return false;

  // Put the BMP to sleep to change settings, then start repeated measurements.
  // Continuous measurements with minimum standby; do not impose a 1-second pause.
  return writeRegister(address, 0xF4, 0x00) &&
         writeRegister(address, 0xF5, 0x00) &&
         writeRegister(address, 0xF4, 0x27);
}

// Look for the BMP280 at either of its two possible addresses.
// Leave bmpAddress at zero if neither address works.
void findBMP(unsigned long now) {
  lastBmpRetry = now;
  bmpAddress = 0;
  if (initializeBMP(0x76)) bmpAddress = 0x76;
  else if (initializeBMP(0x77)) bmpAddress = 0x77;
}

// Read the BMP280 and write the results into temperature (C) and pressure (hPa).
// The & symbols let this function update the caller's two variables.
// Return true only if all checks pass; the caller must discard results on false.
bool readBMP(float &temperature, float &pressure) {
  uint8_t id, control, config, raw[6];
  if (!readRegisters(bmpAddress, 0xD0, &id, 1) || id != 0x58 ||
      !readRegisters(bmpAddress, 0xF4, &control, 1) || control != 0x27 ||
      !readRegisters(bmpAddress, 0xF5, &config, 1) || config != 0x00 ||
      !readRegisters(bmpAddress, 0xF7, raw, sizeof(raw))) return false;

  // A burst read is coherent. Two separate bursts may straddle a new conversion.
  // Join the received bytes into the sensor's raw pressure and temperature.
  uint32_t adcP = (uint32_t(raw[0]) << 12) | (uint32_t(raw[1]) << 4) | (raw[2] >> 4);
  uint32_t adcT = (uint32_t(raw[3]) << 12) | (uint32_t(raw[4]) << 4) | (raw[5] >> 4);
  if (adcP == 0 || adcP == 0xFFFFF || adcP == 0x80000 ||
      adcT == 0 || adcT == 0xFFFFF || adcT == 0x80000) return false;

  // Use the manufacturer's formulas with the factory correction numbers.
  // First calculate temperature, then pressure. Divide Pa by 100 to get hPa.
  float v1 = (adcT / 16384.0f - calibration.t1 / 1024.0f) * calibration.t2;
  float v2 = adcT / 131072.0f - calibration.t1 / 8192.0f;
  v2 = v2 * v2 * calibration.t3;
  float fine = v1 + v2;
  temperature = fine / 5120.0f;
  v1 = fine / 2.0f - 64000.0f;
  v2 = v1 * v1 * calibration.p6 / 32768.0f;
  v2 += v1 * calibration.p5 * 2.0f;
  v2 = v2 / 4.0f + calibration.p4 * 65536.0f;
  v1 = (calibration.p3 * v1 * v1 / 524288.0f + calibration.p2 * v1) / 524288.0f;
  v1 = (1.0f + v1 / 32768.0f) * calibration.p1;
  if (v1 == 0.0f) return false;
  float p = (1048576.0f - adcP - v2 / 4096.0f) * 6250.0f / v1;
  v1 = calibration.p9 * p * p / 2147483648.0f;
  v2 = p * calibration.p8 / 32768.0f;
  pressure = (p + (v1 + v2 + calibration.p7) / 16.0f) / 100.0f;
  return isfinite(temperature) && isfinite(pressure) &&
         temperature >= -40.0f && temperature <= 85.0f &&
         pressure >= 300.0f && pressure <= 1100.0f;
}

// The radio checks the packet CRC; the slave also validates its type and values.
void sendMeasurements(float ds, float bm, float pressure) {
  StationRadio::Packet packet =
      StationRadio::makePacket(StationRadio::MEASUREMENT, ++packetSequence);
  packet.dsTemperature = ds;
  packet.bmpTemperature = bm;
  packet.pressure = pressure;
  lastMeasurementSequence = packet.sequence;
  measurementSent = true;
  sendRadioPacket(packet);
}

// An application reply confirms processing, not just the radio's automatic ACK.
void receiveReplies(unsigned long now) {
  StationRadio::Packet packet;
  // A faulty SPI bus can report an endless stream of available packets.
  for (uint8_t received = 0; received < 3 && radioLink.receive(packet); ++received) {
    if (!StationRadio::validHeader(packet)) {
      continue;
    }
    if (packet.type == StationRadio::MEASUREMENT_ACCEPTED &&
        !resetPending && measurementSent && packet.sequence == lastMeasurementSequence) {
      lastRadioAck = now;
      if (!linkConfirmed) Serial.println(F("SLAVE CONNECTED - measurement accepted"));
      linkConfirmed = true;
    } else if (packet.type == StationRadio::RESET_ACCEPTED &&
               resetPending && !resetAcknowledged && packet.sequence == resetSequence &&
               now - resetAt < RESET_ACK_MS) {
      resetAcknowledged = true;
      resetAcceptedAt = now;
      Serial.println(F("SLAVE RESET ACCEPTED"));
    } else {
      radioLink.reportRejected(packet, F("unexpected reply type/sequence or reset state"));
    }
  }
}

// Watch the master's D2 button. LOW means pressed.
// Ignore quick contact bounces by waiting for a stable reading for 30 ms.
void checkButton(unsigned long now) {
  static uint8_t previous = HIGH, stable = HIGH;
  static unsigned long changedAt = 0;
  uint8_t reading = digitalRead(RESET_BUTTON);
  if (reading != previous) {
    previous = reading;
    changedAt = now;
  }
  if (now - changedAt >= 30 && reading != stable) {
    stable = reading;
    Serial.println(stable == LOW ? F("RESET BUTTON PRESSED (D2)") :
                                  F("RESET BUTTON RELEASED (D2)"));
    if (stable == LOW && !resetPending) {
      resetPending = true;
      resetAcknowledged = false;
      resetAt = now;
      lastResetRequest = now;
      resetSequence = ++packetSequence;
      Serial.println(F("Sending slave reset request"));
      sendRadioPacket(StationRadio::makePacket(StationRadio::RESET_REQUEST, resetSequence));
    }
  }
}

// Retry the reset request if needed, then give the slave time to finish.
// Restart the master even if the slave is disconnected.
void updateReset(unsigned long now) {
  if (!resetAcknowledged && now - resetAt < RESET_ACK_MS &&
      now - lastResetRequest >= 100) {
    sendRadioPacket(StationRadio::makePacket(StationRadio::RESET_REQUEST, resetSequence));
    lastResetRequest = now;
  }
  bool finished = resetAcknowledged ?
      now - resetAcceptedAt >= RESET_FINISH_MS :
      now - resetAt >= RESET_ACK_MS + RESET_FINISH_MS;
  if (finished) {
    Serial.flush();
    wdt_enable(WDTO_15MS);
    while (true) {}
  }
}

// Run once at power-on or after reset.
// Serial is local diagnostics only; all inter-board traffic uses the radio.
void setup() {
  pinMode(RESET_BUTTON, INPUT_PULLUP);
  Serial.begin(115200);
  radioWasReady = radioLink.begin(millis());
  Serial.println(radioWasReady ? F("NRF24 OK - waiting for slave") :
                                F("NRF24 ERROR - check 3.3V/SPI; retrying"));
  Wire.begin();
  Wire.setWireTimeout(3000, true);
  startDS(millis());
  findBMP(millis());
  lastSample = millis();
}

// Run repeatedly: check reset commands and keep DS setup moving.
// Send at 4 Hz; the DS1621 itself converts more slowly than the radio updates.
void loop() {
  unsigned long now = millis();
  serviceRadio(now);
  receiveReplies(now);
  now = millis();
  checkButton(now);
  if (resetPending) {
    updateReset(now);
    return;
  }
  serviceDS(now);
  if (now - lastSample < SAMPLE_MS) return;
  lastSample = now;

  // Read DS independently. A BMP failure below does not erase this DS value.
  float ds = readDS(now);
  float bm = NAN, pressure = NAN;
  if (bmpAddress != 0 && !readBMP(bm, pressure)) {
    // Discard both BMP values on failure, but keep sending the DS result.
    bmpAddress = 0;
    lastBmpRetry = now;
    bm = pressure = NAN;
  } else if (bmpAddress == 0 && now - lastBmpRetry >= RETRY_MS) {
    // Read on the next cycle, after the newly configured sensor has converted.
    findBMP(now);
  }
  sendMeasurements(ds, bm, pressure);
}
