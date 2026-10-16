#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <SoftwareSerial.h>

#define DS1621_ADDR 0x48

SoftwareSerial link(10, 11);  // RX, TX
Adafruit_BMP280 bmp;
bool bmpOK = false;

void setup() {
  Serial.begin(9600);
  link.begin(9600);
  Wire.begin();
  Wire.setWireTimeout(3000, true);
  delay(100);

  Wire.beginTransmission(DS1621_ADDR);
  Wire.write(0xAC);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(20);
  Wire.beginTransmission(DS1621_ADDR);
  Wire.write(0xEE);
  Wire.endTransmission();
  Serial.println("DS1621 started");

  if (bmp.begin(0x76)) {
    Serial.println("BMP280 found at 0x76");
    bmpOK = true;
  } else if (bmp.begin(0x77)) {
    Serial.println("BMP280 found at 0x77");
    bmpOK = true;
  } else {
    Serial.println("BMP280 NOT found");
  }
}

float readDS1621() {
  Wire.beginTransmission(DS1621_ADDR);
  Wire.write(0xAA);
  if (Wire.endTransmission() != 0) return NAN;
  Wire.requestFrom(DS1621_ADDR, 2);
  if (Wire.available() < 2) return NAN;
  int8_t msb = Wire.read();
  uint8_t lsb = Wire.read();
  return msb + ((lsb & 0x80) ? 0.5 : 0.0);
}

void sendField(float v, bool last) {
  if (isnan(v)) link.print("ERR");
  else link.print(v, 1);
  link.print(last ? '\n' : ',');
}

void loop() {
  float tDS = readDS1621();
  float tBMP = NAN, press = NAN;
  if (bmpOK) {
    tBMP = bmp.readTemperature();
    press = bmp.readPressure() / 100.0;
  }

  // Trame envoyee a l'esclave : D,tempDS,tempBMP,pression\n
  link.print("D,");
  sendField(tDS, false);
  sendField(tBMP, false);
  sendField(press, true);

  // Debug USB
  Serial.print("DS1621: "); Serial.print(tDS, 1);
  Serial.print(" | BMP280: "); Serial.print(tBMP, 1);
  Serial.print(" | "); Serial.print(press, 1); Serial.println(" hPa");

  if (Wire.getWireTimeoutFlag()) {
    Serial.println("I2C timeout, bus reset");
    Wire.clearWireTimeoutFlag();
  }

  delay(1000);
}
