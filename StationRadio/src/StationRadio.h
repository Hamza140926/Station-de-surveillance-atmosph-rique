#ifndef STATION_RADIO_H
#define STATION_RADIO_H

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <math.h>

namespace StationRadio {

const uint8_t CE_PIN = 9;
const uint8_t CSN_PIN = 7;
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
      : radio(CE_PIN, CSN_PIN, 4000000), master(master),
        ready(false), lastAttempt(0), lastCheck(0), lastEmptyPoll(0) {}

  bool begin(unsigned long now) {
    ready = false;
    lastAttempt = lastCheck = now;
    // D10 must remain an output for Uno SPI, and deselects the slave's SD card.
    pinMode(10, OUTPUT);
    digitalWrite(10, HIGH);
    pinMode(CSN_PIN, OUTPUT);
    digitalWrite(CSN_PIN, HIGH);
    pinMode(CE_PIN, OUTPUT);
    digitalWrite(CE_PIN, LOW);
    if (!radio.begin()) return false;
    radio.setAutoAck(true);
    radio.setAddressWidth(5);
    radio.setChannel(CHANNEL);
    if (!radio.setDataRate(RF24_1MBPS)) return false;
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
    return true;
  }

  void service(unsigned long now) {
    if (ready && now - lastCheck >= 1000) {
      lastCheck = now;
      if (!radio.isChipConnected()) {
        ready = false;
        lastAttempt = now;
      }
    }
    if (!ready && now - lastAttempt >= RADIO_RETRY_MS) begin(now);
  }

  bool isReady() const { return ready; }

  bool receive(Packet &packet) {
    if (!ready || millis() - lastEmptyPoll < 2) return false;
    if (!radio.available()) {
      lastEmptyPoll = millis();
      return false;
    }
    radio.read(&packet, sizeof(packet));
    return true;
  }

  bool send(const Packet &packet) {
    if (!ready) return false;
    radio.stopListening();
    bool delivered = radio.write(&packet, sizeof(packet));
    radio.startListening();
    return delivered;
  }

 private:
  RF24 radio;
  bool master;
  bool ready;
  unsigned long lastAttempt;
  unsigned long lastCheck;
  unsigned long lastEmptyPoll;
};

}  // namespace StationRadio

#endif
