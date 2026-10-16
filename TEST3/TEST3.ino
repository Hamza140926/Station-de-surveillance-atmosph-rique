
#include <SoftwareSerial.h>

// RX = 0, TX = 1
SoftwareSerial uart(0, 1);

const int ledRouge = 6;
const int ledVerte = 5;

void setup() {
  pinMode(ledRouge, OUTPUT);
  pinMode(ledVerte, OUTPUT);

  // Au demarrage, les deux LED sont eteintes
  digitalWrite(ledRouge, LOW);
  digitalWrite(ledVerte, LOW);

  uart.begin(9600);
}

void loop() {
  if (uart.available() > 0) {
    char commande = uart.read();

    if (commande == 'R') {
      digitalWrite(ledRouge, HIGH);
      digitalWrite(ledVerte, LOW);

      uart.println("R");
    }

    else if (commande == 'V') {
      digitalWrite(ledRouge, LOW);
      digitalWrite(ledVerte, HIGH);

      uart.println("V");
    }
  }
}
