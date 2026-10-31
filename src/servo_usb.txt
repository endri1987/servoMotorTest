#include <Arduino.h>
#include <ESP32Servo.h>

// Test servo me komanda përmes kabllit USB (Serial).
// Mund ta komandosh nga Serial Monitor ose nga faqja web/servo.html (Chrome/Edge).
// Komandat:  start | stop | state | 0..180
// Sa herë ndryshon diçka, ESP32 dërgon një rresht:  STATE angle=90 sweep=0

Servo servo;
const int SERVO_PIN = 27;

bool sweeping = false;          // a po lëviz vetë apo jo
int currentAngle = 90;
const int steps[] = {0, 90, 180, 90};
int stepIndex = 0;
unsigned long lastMoveMs = 0;

void sendState() {
  Serial.printf("STATE angle=%d sweep=%d\n", currentAngle, sweeping ? 1 : 0);
}

void moveTo(int angle) {
  currentAngle = angle;
  servo.write(angle);
  sendState();
}

void handleCommand(String cmd) {
  cmd.trim();
  cmd.toLowerCase();
  if (cmd == "start") {
    sweeping = true;
    sendState();
  } else if (cmd == "stop") {
    sweeping = false;
    sendState();
  } else if (cmd == "state") {
    sendState();
  } else if (cmd.length() > 0 && isDigit(cmd[0])) {
    int angle = cmd.toInt();
    if (angle >= 0 && angle <= 180) {
      sweeping = false;   // kur jep kënd me dorë, ndalon lëvizja automatike
      moveTo(angle);
    } else {
      Serial.println("Këndi duhet të jetë nga 0 deri 180");
    }
  } else if (cmd.length() > 0) {
    Serial.println("Komandat: start, stop, state, ose një numër 0-180");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);                 // mos prit gjatë kur lexon një komandë
  servo.setPeriodHertz(50);              // servot standarde punojnë me 50 Hz
  servo.attach(SERVO_PIN, 500, 2400);    // gjatësia e pulsit për 0° dhe 180°
  Serial.println("Gati. Shkruaj: start, stop, ose një numër 0-180");
  moveTo(90);                            // fillon në mes dhe pret komandë
}

void loop() {
  if (Serial.available()) {
    handleCommand(Serial.readStringUntil('\n'));
  }

  if (sweeping && millis() - lastMoveMs > 1500) {
    lastMoveMs = millis();
    moveTo(steps[stepIndex]);
    stepIndex = (stepIndex + 1) % 4;
  }
}
