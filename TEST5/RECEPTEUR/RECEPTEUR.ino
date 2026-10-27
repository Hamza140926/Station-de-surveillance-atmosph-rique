// Adapted from the Passion-Electronique HelloWorldNRF24L01 example in test.md.
// TEST5 receiver Uno: CE D7, CSN D9, MOSI D11, MISO D12, SCK D13.
#include <SPI.h>
#include <RF24.h>

const uint8_t CE_PIN = 7;
const uint8_t CSN_PIN = 9;
const byte ADDRESS[6] = "PIPE1";
RF24 radio(CE_PIN, CSN_PIN);
char message[33] = {};
unsigned long received = 0;
unsigned long lastReport = 0;
unsigned long lastReceived = 0;

void setup() {
  Serial.begin(115200);
  Serial.println(F("\n[TEST5 RECEPTEUR / SLAVE v2] Serial: 115200 bauds"));
  Serial.print(F("[PINS] CE=D"));
  Serial.print(CE_PIN);
  Serial.print(F(" CSN=D"));
  Serial.print(CSN_PIN);
  Serial.println(F(" MOSI=D11 MISO=D12 SCK=D13"));
  Serial.println(F("[POWER] NRF VCC=3.3V, GND commun avec cet Arduino"));
  // Keep Uno in SPI master mode and deselect any connected SD module.
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
  radio.openReadingPipe(0, ADDRESS);
  radio.startListening();
  Serial.print(F("[CONFIG] Canal lu="));
  Serial.print(radio.getChannel());
  Serial.println(F(" (attendu 76), 1Mbps, PA_MIN, adresse=PIPE1"));
  Serial.println(F("[ECOUTE] Radio locale initialisee; attente de Hello World"));
}

void loop() {
  if (radio.available()) {
    radio.read(message, 32);
    message[32] = '\0';
    ++received;
    lastReceived = millis();
    Serial.print(F("[RX] paquet="));
    Serial.print(received);
    Serial.print(F(" message="));
    Serial.println(message);
  }
  unsigned long now = millis();
  if (now - lastReport >= 5000) {
    lastReport = now;
    Serial.print(F("[ETAT] Radio locale="));
    Serial.print(radio.isChipConnected() ? F("detectee") : F("NON DETECTEE"));
    Serial.print(F(" canal lu="));
    Serial.print(radio.getChannel());
    Serial.print(F(" paquets recus="));
    Serial.print(received);
    if (received == 0) {
      Serial.println(F(" -- aucun message recu depuis le demarrage"));
    } else {
      Serial.print(F(" dernier paquet il y a "));
      Serial.print(now - lastReceived);
      Serial.println(F(" ms"));
    }
  }
}
