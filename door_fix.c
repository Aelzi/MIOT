#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include "pitches.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Firebase_ESP_Client.h>
#include <time.h>

// -------------------- Konfigurasi WiFi & Firebase --------------------
#define WIFI_SSID "Farel"
#define WIFI_PASSWORD "12345678"
#define API_KEY "AIzaSyCQiMXlSh2V1Z49tRO2umBhL7yJVMw7HTg"
#define DATABASE_URL "https://door-acf5f-default-rtdb.asia-southeast1.firebasedatabase.app/" 

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;

// -------------------- Konfigurasi NTP --------------------
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600; // GMT+7
const int daylightOffset_sec = 0;

String getFormattedTime() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
      Serial.println("Failed to obtain time");
      return "";
  }
  char timeString[64];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeString);
}

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

// UID kartu valid
String validUID = "E3 87 26 29";

int failedAttempts = 0;
bool doorIsOpen = false;
bool awaitingCard = false;
bool accessGranted = false;
unsigned long doorOpenTime = 0; 
unsigned long awaitingStartTime = 0; 
const unsigned long WAIT_CARD_TIMEOUT = 10000; // 10 detik

unsigned long lastControlCheck = 0;
const unsigned long CONTROL_CHECK_INTERVAL = 2000;


// -------------------- Fungsi Firebase --------------------
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

void sendFirebaseEvent(const String &path, const String &message) {
  if (Firebase.ready() && signupOK) {
    String timeNow = getFormattedTime();
    FirebaseJson json;
    json.set("message", message);
    json.set("time", timeNow);

    if (!Firebase.RTDB.push(&fbdo, path, &json)) {
      Serial.println("FAILED to send event");
      Serial.println("REASON: " + fbdo.errorReason());
    } else {
      Serial.println("Event sent to Firebase: " + path + " = " + message + " at " + timeNow);
    }
  }
}

// -------------------- Fungsi Nada --------------------
// Nada saat pintu terbuka (naik)
void playDoorOpenTone() {
  tone(buzzerPin, NOTE_C4, 200);
  delay(250);
  tone(buzzerPin, NOTE_D4, 200);
  delay(250);
  tone(buzzerPin, NOTE_E4, 200);
  delay(250);
  noTone(buzzerPin);
}

// Nada saat pintu tertutup (turun)
void playDoorCloseTone() {
  tone(buzzerPin, NOTE_E4, 200);
  delay(250);
  tone(buzzerPin, NOTE_D4, 200);
  delay(250);
  tone(buzzerPin, NOTE_C4, 200);
  delay(250);
  noTone(buzzerPin);
}

// Nada saat kartu invalid
void playInvalidCardTone() {
  // Dua beep bernada tinggi
  tone(buzzerPin, NOTE_C5, 200);
  delay(300);
  noTone(buzzerPin);
  delay(100);
  tone(buzzerPin, NOTE_C5, 200);
  delay(300);
  noTone(buzzerPin);
}

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

void resetToIdle() {
  awaitingCard = false;
  accessGranted = false;
  displayMessage("Pintu Otomatis", "Menunggu...");
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
    sendFirebaseEvent("events/validAccess", "UID: " + readUID + " Access Granted");
    return true;
  } else {
    Serial.println("UID is not valid!");
    sendFirebaseEvent("events/invalidAccess", "UID: " + readUID + " Access Denied");
    return false;
  }
}

void openDoor() {
  digitalWrite(RelayDoorPin, LOW);
  for (int pos = 0; pos <= 90; pos += 5) {
    servo1.write(pos);
    delay(20);
  }
  doorIsOpen = true;
  doorOpenTime = millis();
  Serial.println("Door opened");
  sendFirebaseEvent("events/doorStatus", "Door Opened");
  playDoorOpenTone(); // Mainkan nada pintu terbuka
  //resetToIdle();
}

void closeDoor() {
  for (int pos = 90; pos >= 0; pos -= 5) {
    servo1.write(pos);
    delay(20);
  }
  digitalWrite(RelayDoorPin, HIGH);
  doorIsOpen = false;
  Serial.println("Door closed");
  sendFirebaseEvent("events/doorStatus", "Door Closed");
  playDoorCloseTone(); // Mainkan nada pintu tertutup
  resetToIdle();
}

void denyAccessSequence() {
  digitalWrite(ledMerahPin, HIGH);
  displayMessage("Akses Ditolak!", "Kartu Invalid");
  Serial.println("Access denied: Invalid card");
  playInvalidCardTone(); // Nada untuk kartu invalid
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
  rfid.PCD_Init();
  Serial.println("RFID Scanner Enabled again.");
}

void escalateFailedAttempts() {
  failedAttempts++;
  sendFirebaseEvent("events/failedAttempts", "Failed Attempts: " + String(failedAttempts));
  switch (failedAttempts) {
    case 1:
      denyAccessSequence();
      break;
    case 2:
      denyAccessSequence();
      Serial.println("Attempt 2");
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
  sendFirebaseEvent("events/suspiciousActivity", "Suspicious vibration detected");
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
  sendFirebaseEvent("events/forcedEntry", "Door forced open!");
  displayMessage("ALARM!", "Pintu Dibuka Paksa");
  unsigned long startAlarm = millis();
  while (millis() - startAlarm < 10000) { // Alarm 10 detik saja
    digitalWrite(ledMerahPin, HIGH);
    tone(buzzerPin, NOTE_C5, 500);
    delay(500);
    digitalWrite(ledMerahPin, LOW);
    noTone(buzzerPin);
    delay(500);
  }
  resetToIdle();
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

  digitalWrite(RelayDoorPin, HIGH);
  servo1.attach(servoPin);
  servo1.write(0);

  SPI.begin();
  rfid.PCD_Init();
  resetToIdle();
  Serial.println("System Ready.");

  // Koneksi WiFi
  Serial.print("Connecting to Wi-Fi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED){
    Serial.print(".");
    delay(300);
  }
  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());

  // Sinkronisasi NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
  } else {
    Serial.println("Time synchronized");
  }

  // Setup Firebase
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  if (Firebase.signUp(&config, &auth, "", "")){
    Serial.println("Firebase signup success");
    signupOK = true;
  } else {
    Serial.printf("Firebase signup error: %s\n", config.signer.signupError.message.c_str());
  }

  config.token_status_callback = tokenStatusCallback; 
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

// -------------------- LOOP --------------------
void loop() {
  // Cek kontrol pintu dari Firebase
  //resetToIdle();
  if (Firebase.ready() && signupOK && (millis() - lastControlCheck > CONTROL_CHECK_INTERVAL)) {
    lastControlCheck = millis();
    if (Firebase.RTDB.getString(&fbdo, "control/doorCommand")) {
      String doorCommand = fbdo.stringData();
      if (doorCommand == "open" && !doorIsOpen) {
        Serial.println("Firebase command: OPEN DOOR");
        openDoor();
      } else if (doorCommand == "close" && doorIsOpen) {
        Serial.println("Firebase command: CLOSE DOOR");
        closeDoor();
      }
    }
  }

  // Cek PIR
  if (!accessGranted && !doorIsOpen && !awaitingCard) {
    if (digitalRead(PirSensorPin) == HIGH) {
      awaitingCard = true;
      awaitingStartTime = millis();
      displayMessage("Silakan Scan", "Kartu Anggota");
      Serial.println("Movement detected. Awaiting card...");
    }
  }

  // Menunggu kartu
  if (awaitingCard) {
    if (millis() - awaitingStartTime > WAIT_CARD_TIMEOUT) {
      Serial.println("No card detected within timeout. Returning to idle.");
      resetToIdle();
      rfid.PCD_Init();
    } else {
      String readUID = readCardUID();
      if (readUID != "") {
        Serial.print("Detected UID: ");
        Serial.println(readUID);
        if (readUID.equals(validUID)) {
          awaitingCard = false;
          accessGranted = true;
          failedAttempts = 0;
          displayMessage("Akses Disetujui");
          digitalWrite(ledHijauPin, HIGH);
          openDoor(); 
          delay(2000);
        } else {
          awaitingCard = false;
          accessGranted = false;
          escalateFailedAttempts();
          rfid.PCD_Init();
          resetToIdle();
        }
      }
    }
  }

  // Tutup pintu otomatis setelah 5 detik jika terbuka
  if (accessGranted && doorIsOpen) {
    if (millis() - doorOpenTime > 5000) {
      displayMessage("Menutup Pintu");
      closeDoor();
      digitalWrite(ledHijauPin, LOW);
      accessGranted = false;
      rfid.PCD_Init();
      resetToIdle();
    }
  }

  // Cek getaran mencurigakan
  if (isSuspiciousVibration()) {
    alarmSuspiciousActivity();
  }

  // Cek pembukaan paksa pintu
  if (isDoorForcedOpen()) {
    alarmForcedEntry(); 
  }

  delay(1000);
}
