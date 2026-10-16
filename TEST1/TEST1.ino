
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

const int ledRouge = 6;
const int ledVerte = 5;
const int bouton = 4;

bool rougeAllumee = false;

int ancienEtatBouton = HIGH;

void setup() {
  pinMode(ledRouge, OUTPUT);
  pinMode(ledVerte, OUTPUT);
  pinMode(bouton, INPUT_PULLUP);

  // Etat initial : LED verte allumee
  digitalWrite(ledRouge, LOW);
  digitalWrite(ledVerte, HIGH);

  Serial.begin(9600);

  // Initialisation du LCD
  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("Test Arduino");
  lcd.setCursor(0, 1);
  lcd.print("LED VERTE");

  Serial.println("Systeme demarre");
  Serial.println("LED verte allumee");
}

void loop() {
  int etatBouton = digitalRead(bouton);

  // Detecter un nouvel appui
  if (etatBouton == LOW && ancienEtatBouton == HIGH) {
    delay(30); // Anti-rebond simple

    if (digitalRead(bouton) == LOW) {
      rougeAllumee = !rougeAllumee;

      if (rougeAllumee) {
        digitalWrite(ledRouge, HIGH);
        digitalWrite(ledVerte, LOW);

        lcd.setCursor(0, 0);
        lcd.print("Test Arduino   ");
        lcd.setCursor(0, 1);
        lcd.print("LED ROUGE      ");

        Serial.println("LED rouge allumee");
      } else {
        digitalWrite(ledRouge, LOW);
        digitalWrite(ledVerte, HIGH);

        lcd.setCursor(0, 0);
        lcd.print("Test Arduino   ");
        lcd.setCursor(0, 1);
        lcd.print("LED VERTE      ");

        Serial.println("LED verte allumee");
      }

      // Attendre le relachement du bouton
      while (digitalRead(bouton) == LOW) {
        delay(10);
      }
    }
  }

  ancienEtatBouton = etatBouton;
}
