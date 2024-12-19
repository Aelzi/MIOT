#include <Arduino.h>
#include <Wire.h>

// Define pin constants
const int Relay = 13;
const int Trig = 14; // Trig pin of ultrasonic sensor
const int Echo = 27; // Echo pin of ultrasonic sensor
const int Button = 12; // Button pin

// Variables to track button state
int lastState = HIGH; // Previous state of the button
int currentState;     // Current reading of the button state

void setup() {
  // Initialize serial communication
  Serial.begin(9600);

  // Set pin modes
  pinMode(Relay, OUTPUT);
  pinMode(Trig, OUTPUT);
  pinMode(Echo, INPUT);
  pinMode(Button, INPUT_PULLUP);

  // Ensure relay is off at startup
  digitalWrite(Relay, LOW);
}

void loop() {
  // Read the state of the button
  currentState = digitalRead(Button);

  // Trigger the ultrasonic sensor
  digitalWrite(Trig, LOW);
  delayMicroseconds(2);
  digitalWrite(Trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(Trig, LOW);

  // Measure the duration of the echo signal
  long duration = pulseIn(Echo, HIGH);

  // Calculate distance in cm
  int distance = duration * 0.034 / 2;

  // Print the distance to the serial monitor
  Serial.print("Distance: ");
  Serial.print(distance);
  Serial.println(" cm");

  // Control relay based on distance
  if (distance > 5) {
    Serial.println("Distance > 5 cm: Relay ON");
    digitalWrite(Relay, HIGH); // Turn on the relay
  } else {
    Serial.println("Distance <= 5 cm: Relay OFF");
    digitalWrite(Relay, LOW); // Turn off the relay
  }

  // Check if the button is pressed (state changes from HIGH to LOW)
  if (lastState == HIGH && currentState == LOW) {
    Serial.println("Button pressed: Toggling Relay");
    digitalWrite(Relay, !digitalRead(Relay)); // Toggle relay state
  }

  // Update the last state
  lastState = currentState;

  // Small delay to debounce button
  delay(50);

  // Wait before the next measurement
  delay(100);
}
