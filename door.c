#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include "pitches.h"

// -------------------- Konfigurasi Pin --------------------
const int buzzerPin = 4; 
const int ledMerahPin = 0;   
const int ledHijauPin = 2;   
const int RelayDoorPin = 16; 
const int DoorSensorPin = 17;
const int VibrationSensorPin = 25;
const int PirSensorPin = 33;
const int servoPin = 13;

#define SS_PIN  5    // SDA pada RFID
#define RST_PIN 26   // RST pada RFID

// -------------------- Variabel Global --------------------
Servo servo1;
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 rfid(SS_PIN, RST_PIN);

// UID kartu yang dianggap valid
String validUID = "E3 87 26 29";

int failedAttempts = 0;
bool doorIsOpen = false;
bool awaitingCard = false;
bool accessGranted = false;
unsigned long doorOpenTime = 0; 
unsigned long awaitingStartTime = 0; // Waktu mulai menunggu kartu

// Timeout menunggu kartu (ms)
const unsigned long WAIT_CARD_TIMEOUT = 10000; // 10 detik

// -------------------- Fungsi Bantu --------------------
void displayMessage(const char* line1, const char* line2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  if (strlen(line2) > 0) {
    lcd.setCursor(0, 1);
    lcd.print(line2);
  }
}

bool isDoorForcedOpen() {
  int doorState = digitalRead(DoorSensorPin);
  if (doorState == HIGH && !doorIsOpen && !accessGranted) {
    return true;
  }
  return false;
}

bool isSuspiciousVibration() {
  return (digitalRead(VibrationSensorPin) == HIGH);
}

String readCardUID() {
  if (!rfid.PICC_IsNewCardPresent()) return "";
  if (!rfid.PICC_ReadCardSerial()) return "";

  String readUID = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) {
      readUID += "0";
    }
    readUID += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) {
      readUID += " ";
    }
  }
  
  readUID.toUpperCase();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  return readUID;
}

bool isCardValid() {
  String readUID = readCardUID();
  if (readUID == "") return false;

  Serial.print("Detected UID: ");
  Serial.println(readUID);

  if (readUID.equals(validUID)) {
    Serial.println("UID is valid!");
    return true;
  } else {
    Serial.println("UID is not valid!");
    return false;
  }
}

void openDoor() {
  digitalWrite(RelayDoorPin, LOW); // Buka kunci (aktif low)
  for (int pos = 0; pos <= 90; pos += 5) {
    servo1.write(pos);
    delay(20);
  }
  doorIsOpen = true;
  doorOpenTime = millis();
  Serial.println("Door opened");
}

void closeDoor() {
  for (int pos = 90; pos >= 0; pos -= 5) {
    servo1.write(pos);
    delay(20);
  }
  digitalWrite(RelayDoorPin, HIGH); // Kunci pintu (nonaktif relay)
  doorIsOpen = false;
  Serial.println("Door closed");
}

void denyAccessSequence() {
  digitalWrite(ledMerahPin, HIGH);
  displayMessage("Akses Ditolak!", "Kartu Invalid");
  Serial.println("Access denied: Invalid card");
  delay(2000);
  digitalWrite(ledMerahPin, LOW);
}

void countdownRFIDOff() {
  for (int i = 30; i > 0; i--) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("RFID Off: ");
    lcd.print(i);
    lcd.print("s");

    Serial.print("RFID disabled for ");
    Serial.print(i);
    Serial.println(" seconds...");
    delay(1000);
  }
  
  // Re-inisialisasi RFID
  rfid.PCD_Init();
  Serial.println("RFID Scanner Enabled again.");
}

void escalateFailedAttempts() {
  failedAttempts++;
  switch (failedAttempts) {
    case 1:
      denyAccessSequence();
      break;
    case 2:
      denyAccessSequence();
      Serial.println("Attempt 2: Notified on Serial Monitor.");
      break;
    case 3:
      denyAccessSequence();
      displayMessage("RFID Off 30s", "Tunggu...");
      Serial.println("RFID Scanner Disabled for 30 seconds.");
      countdownRFIDOff();
      break;
    case 4:
      denyAccessSequence();
      Serial.println("Attempt 4: Buzzer alarm triggered.");
      for (int i = 0; i < 5; i++) {
        tone(buzzerPin, NOTE_A4, 500);
        delay(500);
        noTone(buzzerPin);
      }
      break;
    default:
      denyAccessSequence();
      break;
  }
}

void alarmSuspiciousActivity() {
  Serial.println("Suspicious activity detected!");
  displayMessage("ALARM!", "Getaran Terdeteksi");
  for (int i = 0; i < 5; i++) {
    digitalWrite(ledMerahPin, HIGH);
    tone(buzzerPin, NOTE_C6, 200);
    delay(200);
    noTone(buzzerPin);
    digitalWrite(ledMerahPin, LOW);
    delay(200);
  }
  displayMessage("Pintu Otomatis", "Menunggu...");
}

void alarmForcedEntry() {
  Serial.println("Forced Entry Detected!");
  displayMessage("ALARM!", "Pintu Dibuka Paksa");
  while (true) {
    digitalWrite(ledMerahPin, HIGH);
    tone(buzzerPin, NOTE_C5, 500);
    delay(500);
    digitalWrite(ledMerahPin, LOW);
    noTone(buzzerPin);
    delay(500);
  }
}

// Mengembalikan sistem ke kondisi menunggu PIR lagi
void resetToIdle() {
  awaitingCard = false;
  accessGranted = false;
  displayMessage("Pintu Otomatis", "Menunggu...");
}

// -------------------- SETUP --------------------
void setup() {
  Serial.begin(9600);
  lcd.init();
  lcd.backlight();

  pinMode(ledMerahPin, OUTPUT);
  pinMode(ledHijauPin, OUTPUT);
  pinMode(buzzerPin, OUTPUT);
  pinMode(RelayDoorPin, OUTPUT);
  pinMode(DoorSensorPin, INPUT_PULLUP);
  pinMode(VibrationSensorPin, INPUT);
  pinMode(PirSensorPin, INPUT);

  // Pintu terkunci di awal
  digitalWrite(RelayDoorPin, HIGH);

  servo1.attach(servoPin);
  servo1.write(0);

  SPI.begin();
  rfid.PCD_Init();
  
  resetToIdle();
  Serial.println("System Ready.");
}

// -------------------- LOOP --------------------
void loop() {
  // Cek PIR
  if (!accessGranted && !doorIsOpen && !awaitingCard) {
    // Jika PIR mendeteksi gerakan, mulai menunggu kartu
    if (digitalRead(PirSensorPin) == HIGH) {
      awaitingCard = true;
      awaitingStartTime = millis();
      displayMessage("Silakan Scan", "Kartu Anggota");
      Serial.println("Movement detected. Awaiting card...");
    }
  }

  // Jika menunggu kartu
  if (awaitingCard) {
    // Timeout: jika dalam 10 detik tidak ada kartu terbaca, kembali ke menunggu PIR
    if (millis() - awaitingStartTime > WAIT_CARD_TIMEOUT) {
      Serial.println("No card detected within timeout. Returning to idle.");
      resetToIdle();
      rfid.PCD_Init(); // Re-init RFID untuk kestabilan
    } else {
      // Coba baca kartu
      String readUID = readCardUID();
      if (readUID != "") {
        Serial.print("Detected UID: ");
        Serial.println(readUID);
        if (readUID.equals(validUID)) {
          // Kartu valid
          awaitingCard = false;
          accessGranted = true;
          failedAttempts = 0;
          displayMessage("Akses Disetujui");
          digitalWrite(ledHijauPin, HIGH);
          openDoor(); 
          delay(2000);
        } else {
          // Kartu tidak valid
          awaitingCard = false;
          accessGranted = false;
          escalateFailedAttempts();
          rfid.PCD_Init(); // Re-init RFID setiap selesai upaya akses
          resetToIdle();
        }
      }
    }
  }

  // Jika akses disetujui dan pintu terbuka, tutup pintu setelah beberapa detik
  if (accessGranted && doorIsOpen) {
    if (millis() - doorOpenTime > 5000) {
      closeDoor();
      digitalWrite(ledHijauPin, LOW);
      accessGranted = false;
      rfid.PCD_Init(); // Re-init RFID agar siap siklus selanjutnya
      resetToIdle();
    }
  }

  // Cek getaran
  if (isSuspiciousVibration()) {
    alarmSuspiciousActivity();
  }

  // Cek pembukaan paksa pintu
  if (isDoorForcedOpen()) {
    alarmForcedEntry(); // Tidak kembali
  }

  delay(100); // Delay kecil agar loop tidak terlalu cepat
}
