#ifndef STATION_RADIO_H
#define STATION_RADIO_H

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <math.h>

namespace StationRadio {

const uint8_t CE_PIN = 9;
const uint8_t CSN_PIN = 7;
const uint8_t MASTER_CE_PIN = 8;
const uint8_t MASTER_CSN_PIN = 10;
const uint8_t CHANNEL = 76;
const unsigned long LINK_TIMEOUT_MS = 3500;
const unsigned long RADIO_RETRY_MS = 3000;
const uint16_t PACKET_MAGIC = 0xA724;
const uint8_t PROTOCOL_VERSION = 1;
const uint8_t MASTER_ADDRESS[6] = "ATM01";
const uint8_t SLAVE_ADDRESS[6] = "ATM02";

enum MessageType : uint8_t {
  MEASUREMENT = 1,
  MEASUREMENT_ACCEPTED = 2,
  RESET_REQUEST = 3,
  RESET_ACCEPTED = 4
};

// Both Uno boards use the same 20-byte layout, below the radio's 32-byte limit.
struct Packet {
  uint32_t sequence;
  float dsTemperature;
  float bmpTemperature;
  float pressure;
  uint16_t magic;
  uint8_t version;
  uint8_t type;
};

static_assert(sizeof(float) == 4, "The radio protocol requires 32-bit floats");
static_assert(sizeof(Packet) == 20, "The radio packet must be exactly 20 bytes");

inline Packet makePacket(MessageType type, uint32_t sequence) {
  Packet packet = {};
  packet.sequence = sequence;
  packet.magic = PACKET_MAGIC;
  packet.version = PROTOCOL_VERSION;
  packet.type = type;
  return packet;
}

inline bool validHeader(const Packet &packet) {
  return packet.magic == PACKET_MAGIC && packet.version == PROTOCOL_VERSION &&
         packet.type >= MEASUREMENT && packet.type <= RESET_ACCEPTED;
}

inline bool validValue(float value, float minimum, float maximum) {
  return isnan(value) || (isfinite(value) && value >= minimum && value <= maximum);
}

inline bool validMeasurement(const Packet &packet) {
  return validHeader(packet) && packet.type == MEASUREMENT &&
         validValue(packet.dsTemperature, -55.0f, 125.0f) &&
         validValue(packet.bmpTemperature, -40.0f, 85.0f) &&
         validValue(packet.pressure, 300.0f, 1100.0f) &&
         isnan(packet.bmpTemperature) == isnan(packet.pressure);
}

class Link {
 public:
  explicit Link(bool master)
      : radio(master ? MASTER_CE_PIN : CE_PIN,
              master ? MASTER_CSN_PIN : CSN_PIN, 4000000), master(master),
        ready(false), lastAttempt(0), lastCheck(0), lastEmptyPoll(0),
        lastReport(0), lastInvalidReport(0), txCount(0), ackCount(0),
        rxCount(0), validHeaderCount(0), rejectedCount(0) {}

  bool begin(unsigned long now) {
    ready = false;
    lastAttempt = lastCheck = now;
    Serial.println(master ? F("[BOOT MASTER] StationRadio diagnostic v2") :
                            F("[BOOT SLAVE] StationRadio diagnostic v2"));
    Serial.print(F("[NRF INIT] CE=D"));
    Serial.print(master ? MASTER_CE_PIN : CE_PIN);
    Serial.print(F(" CSN=D"));
    Serial.print(master ? MASTER_CSN_PIN : CSN_PIN);
    Serial.println(F(" MOSI=D11 MISO=D12 SCK=D13; VCC=3.3V"));
    // D10 must remain an output for Uno SPI, and deselects the slave's SD card.
    pinMode(10, OUTPUT);
    digitalWrite(10, HIGH);
    const uint8_t csnPin = master ? MASTER_CSN_PIN : CSN_PIN;
    const uint8_t cePin = master ? MASTER_CE_PIN : CE_PIN;
    pinMode(csnPin, OUTPUT);
    digitalWrite(csnPin, HIGH);
    pinMode(cePin, OUTPUT);
    digitalWrite(cePin, LOW);
    if (!radio.begin()) {
      Serial.println(F("[NRF ERROR] begin failed: check local power, GND and SPI wiring"));
      return false;
    }
    radio.setAutoAck(true);
    radio.setAddressWidth(5);
    radio.setChannel(CHANNEL);
    if (!radio.setDataRate(RF24_1MBPS)) {
      Serial.println(F("[NRF ERROR] 1Mbps setting failed: local SPI/configuration problem"));
      return false;
    }
    radio.setPALevel(RF24_PA_LOW);
    radio.setCRCLength(RF24_CRC_16);
    radio.disableDynamicPayloads();
    radio.setPayloadSize(sizeof(Packet));
    radio.setRetries(5, 5);
    radio.openWritingPipe(master ? SLAVE_ADDRESS : MASTER_ADDRESS);
    radio.openReadingPipe(1, master ? MASTER_ADDRESS : SLAVE_ADDRESS);
    radio.flush_rx();
    radio.startListening();
    ready = true;
    printConfiguration();
    Serial.println(F("[NRF INIT] Local radio ready; remote connection NOT confirmed"));
    return true;
  }

  void service(unsigned long now) {
    if (ready && now - lastCheck >= 1000) {
      lastCheck = now;
      if (!radio.isChipConnected()) {
        Serial.println(F("[NRF ERROR] Local SPI presence lost; retry in 3s"));
        ready = false;
        lastAttempt = now;
      }
    }
    if (!ready && now - lastAttempt >= RADIO_RETRY_MS) begin(now);
    if (now - lastReport >= 5000) {
      lastReport = now;
      Serial.print(F("[NRF STATS] ms="));
      Serial.print(now);
      Serial.print(F(" ready="));
      Serial.print(ready);
      Serial.print(F(" tx="));
      Serial.print(txCount);
      Serial.print(F(" ack="));
      Serial.print(ackCount);
      Serial.print(F(" fail="));
      Serial.print(txCount - ackCount);
      Serial.print(F(" rx="));
      Serial.print(rxCount);
      Serial.print(F(" headerOK="));
      Serial.print(validHeaderCount);
      Serial.print(F(" rejected="));
      Serial.println(rejectedCount);
      if (ready) printConfiguration();
    }
  }

  bool isReady() const { return ready; }

  bool receive(Packet &packet) {
    if (!ready || millis() - lastEmptyPoll < 2) return false;
    if (!radio.available()) {
      lastEmptyPoll = millis();
      return false;
    }
    radio.read(&packet, sizeof(packet));
    ++rxCount;
    if (validHeader(packet)) ++validHeaderCount;
    else reportRejected(packet, F("invalid header; expected magic=A724 version=1 type=1..4"));
    return true;
  }

  void reportRejected(const Packet &packet, const __FlashStringHelper *reason) {
    ++rejectedCount;
    unsigned long now = millis();
    if (rejectedCount != 1 && now - lastInvalidReport < 5000) return;
    lastInvalidReport = now;
    Serial.print(F("[NRF RX REJECTED] "));
    Serial.println(reason);
    Serial.print(F("seq="));
    Serial.print(packet.sequence);
    Serial.print(F(" magic=0x"));
    Serial.print(packet.magic, HEX);
    Serial.print(F(" version="));
    Serial.print(packet.version);
    Serial.print(F(" type="));
    Serial.println(packet.type);
    Serial.print(F("raw="));
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&packet);
    for (uint8_t i = 0; i < sizeof(packet); ++i) {
      if (bytes[i] < 16) Serial.print('0');
      Serial.print(bytes[i], HEX);
      Serial.print(' ');
    }
    Serial.println();
    Serial.println(F("[NRF HINT] Repeated 00/FF: check SPI/power; otherwise compare firmware/library"));
  }

  bool send(const Packet &packet) {
    if (!ready) return false;
    ++txCount;
    radio.stopListening();
    bool delivered = radio.write(&packet, sizeof(packet));
    radio.startListening();
    if (delivered) ++ackCount;
    return delivered;
  }

 private:
  void printConfiguration() {
    Serial.print(F("[NRF CONFIG] channel="));
    Serial.print(radio.getChannel());
    Serial.print(F("/76 rate="));
    Serial.print(radio.getDataRate() == RF24_1MBPS ? F("1Mbps") : F("WRONG"));
    Serial.print(F(" payload="));
    Serial.print(radio.getPayloadSize());
    Serial.print(F("/20 TX="));
    Serial.print(master ? F("ATM02") : F("ATM01"));
    Serial.print(F(" RX=pipe1:"));
    Serial.println(master ? F("ATM01") : F("ATM02"));
  }

  RF24 radio;
  bool master;
  bool ready;
  unsigned long lastAttempt;
  unsigned long lastCheck;
  unsigned long lastEmptyPoll;
  unsigned long lastReport;
  unsigned long lastInvalidReport;
  uint32_t txCount;
  uint32_t ackCount;
  uint32_t rxCount;
  uint32_t validHeaderCount;
  uint32_t rejectedCount;
};

}  // namespace StationRadio

#endif
