// Adapted from the Passion-Electronique HelloWorldNRF24L01 example in test.md.
// TEST5 transmitter Uno: CE D7, CSN D8, MOSI D11, MISO D12, SCK D13.
#include <SPI.h>
#include <RF24.h>

const uint8_t CE_PIN = 7;
const uint8_t CSN_PIN = 8;
const byte ADDRESS[6] = "PIPE1";
const char MESSAGE[] = "Hello World !!!";
RF24 radio(CE_PIN, CSN_PIN);
unsigned long attempts = 0;
unsigned long acknowledged = 0;

void setup() {
  Serial.begin(115200);
  Serial.println(F("\n[TEST5 EMETTEUR / MASTER v2] Serial: 115200 bauds"));
  Serial.print(F("[PINS] CE=D"));
  Serial.print(CE_PIN);
  Serial.print(F(" CSN=D"));
  Serial.print(CSN_PIN);
  Serial.println(F(" MOSI=D11 MISO=D12 SCK=D13"));
  Serial.println(F("[POWER] NRF VCC=3.3V, GND commun avec cet Arduino"));
  pinMode(10, OUTPUT);
  digitalWrite(10, HIGH);
  if (!radio.begin()) {
    Serial.println(F("[ERREUR INIT] Radio locale non detectee: alimentation ou SPI"));
    Serial.println(F("[ARRET] Corriger le probleme puis redemarrer cette carte"));
    while (true) delay(1000);
  }
  radio.setChannel(76);
  if (!radio.setDataRate(RF24_1MBPS)) {
    Serial.println(F("[ERREUR CONFIG] Impossible de regler 1Mbps; redemarrer apres verification"));
    while (true) delay(1000);
  }
  radio.setAutoAck(true);
  radio.setPALevel(RF24_PA_MIN);
  radio.openWritingPipe(ADDRESS);
  radio.stopListening();
  Serial.print(F("[CONFIG] Canal lu="));
  Serial.print(radio.getChannel());
  Serial.println(F(" (attendu 76), 1Mbps, PA_MIN, adresse=PIPE1"));
  Serial.println(F("[PRET] Radio locale initialisee; liaison distante NON confirmee"));
}

void loop() {
  ++attempts;
  bool delivered = radio.write(MESSAGE, sizeof(MESSAGE));
  if (delivered) ++acknowledged;
  Serial.print(F("[TX] essai="));
  Serial.print(attempts);
  Serial.print(delivered ? F(" ACK OK") : F(" ECHEC: aucun ACK du recepteur"));
  Serial.print(F(" succes="));
  Serial.print(acknowledged);
  Serial.print(F(" echecs="));
  Serial.println(attempts - acknowledged);
  if (attempts % 5 == 0) {
    Serial.print(F("[SPI] Radio locale="));
    Serial.print(radio.isChipConnected() ? F("detectee") : F("NON DETECTEE"));
    Serial.print(F(" canal lu="));
    Serial.println(radio.getChannel());
  }
  delay(1000);
}
