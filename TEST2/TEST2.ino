
#include <SoftwareSerial.h>

// RX = 0, TX = 1
SoftwareSerial uart(0, 1);

void setup() {
  Serial.begin(9600);
  uart.begin(9600);

  Serial.println("=== ARDUINO MASTER ===");
  Serial.println("Envoyer R pour la LED rouge");
  Serial.println("Envoyer V pour la LED verte");
}

void loop() {
  if (Serial.available() > 0) {
    char commande = Serial.read();

    // Ignorer les fins de ligne
    if (commande == '\n' || commande == '\r') {
      return;
    }

    // Accepter r et v minuscules
    if (commande >= 'a' && commande <= 'z') {
      commande = commande - 32;
    }

    if (commande == 'R' || commande == 'V') {
      uart.write(commande);

      Serial.print("Commande envoyee : ");
      Serial.println(commande);
    } else {
      Serial.println("Commande invalide. Utiliser R ou V.");
    }
  }

  // Lire la reponse du Slave
  if (uart.available() > 0) {
    char reponse = uart.read();
    Serial.print("Reponse du Slave : ");
    Serial.println(reponse);
  }
}
