#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <ESP32Servo.h>
#include "secrets.h"

// Servo me faqe web, në çfarëdo shtëpie:
//  - Herën e parë (ose në shtëpi të re) ESP32 hap WiFi-n "servoLocal".
//    Lidhu me telefon, zgjidh WiFi-n e shtëpisë, shkruaj fjalëkalimin, Save.
//  - Pastaj hap në browser:  http://servo.local  (ose IP-në nga Serial Monitor)
//  - Mbaj shtypur butonin BOOT 3 sekonda -> harron WiFi-n dhe hap "servoLocal" përsëri.
// Serial Monitor:  start | stop | state | resetwifi | 0..180

// ---------- CILËSIMET ----------
const int SERVO_PIN = 27;
const int BOOT_PIN  = 0;              // butoni BOOT i pllakës
const char* HOSTNAME = "servo";       // -> http://servo.local

// Ekrani LED me 4 shifra (TM1637)
const int TM_DIO = 14;
const int TM_CLK = 26;
const int TM_NDRICIMI = 4;            // ndriçimi 0..7
// --------------------------------

Servo servo;
WebServer server(80);
WiFiManager wm;

bool sweeping = false;          // a po lëviz vetë apo jo
int currentAngle = 90;
const int steps[] = {0, 90, 180, 90};
int stepIndex = 0;
unsigned long lastMoveMs = 0;
unsigned long bootPressedMs = 0;

// Faqja web (HTML + CSS + JavaScript), ruhet brenda ESP32
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="sq">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Servo</title>
<style>
  body { font-family: sans-serif; background: #1e1e2e; color: #eee; text-align: center; margin: 0; padding: 20px; }
  h1 { font-size: 22px; }
  .card { background: #2a2a3d; border-radius: 16px; padding: 20px; max-width: 360px; margin: 0 auto; }
  #arm { transition: transform 0.4s ease; transform-origin: 150px 150px; }
  .angle { font-size: 40px; font-weight: bold; margin: 5px 0; }
  .status { color: #aaa; margin-bottom: 15px; }
  .offline { color: #f66; }
  input[type=range] { width: 100%; margin: 15px 0; }
  button { font-size: 16px; padding: 12px 0; margin: 4px; border: none; border-radius: 10px; color: #fff; width: 30%; }
  .go { background: #2e9e5b; } .stop { background: #c0392b; } .pos { background: #3b6fd8; }
</style>
</head>
<body>
<div class="card">
  <h1>Kontrolli i Servos</h1>
  <svg viewBox="0 0 300 170" width="100%">
    <path d="M 30 150 A 120 120 0 0 1 270 150" fill="none" stroke="#555" stroke-width="4"/>
    <text x="20" y="168" fill="#888" font-size="12">0°</text>
    <text x="140" y="22" fill="#888" font-size="12">90°</text>
    <text x="262" y="168" fill="#888" font-size="12">180°</text>
    <g id="arm">
      <rect x="144" y="45" width="12" height="105" rx="6" fill="#f5a623"/>
    </g>
    <circle cx="150" cy="150" r="16" fill="#ddd"/>
  </svg>
  <div class="angle"><span id="deg">90</span>°</div>
  <div class="status" id="status">Po lidhem...</div>
  <input type="range" id="slider" min="0" max="180" value="90">
  <div>
    <button class="go" onclick="cmd('/start')">Start</button>
    <button class="stop" onclick="cmd('/stop')">Stop</button>
  </div>
  <div>
    <button class="pos" onclick="setAngle(0)">0°</button>
    <button class="pos" onclick="setAngle(90)">90°</button>
    <button class="pos" onclick="setAngle(180)">180°</button>
  </div>
</div>
<script>
  const slider = document.getElementById('slider');
  const status = document.getElementById('status');
  let dragging = false, lastSend = 0;

  // Vizatimi: 0° = majtas, 90° = lart, 180° = djathtas
  function show(angle, sweeping) {
    document.getElementById('arm').style.transform = 'rotate(' + (angle - 90) + 'deg)';
    document.getElementById('deg').textContent = angle;
    status.textContent = sweeping ? 'Po lëviz vetë (Start)' : 'Në pritje';
    status.className = 'status';
    if (!dragging) slider.value = angle;
  }

  function cmd(url) {
    fetch(url).then(r => r.json()).then(s => show(s.angle, s.sweeping))
      .catch(() => { status.textContent = 'ESP32 nuk përgjigjet'; status.className = 'status offline'; });
  }

  function setAngle(a) { cmd('/set?angle=' + a); }

  // Rrëshqitësi: vizatimi lëviz menjëherë, komanda dërgohet çdo 100 ms
  slider.addEventListener('input', () => {
    dragging = true;
    show(+slider.value, false);
    if (Date.now() - lastSend > 100) { lastSend = Date.now(); setAngle(slider.value); }
  });
  slider.addEventListener('change', () => { dragging = false; setAngle(slider.value); });

  // Pyet ESP32 çdo gjysmë sekonde se ku është servo
  setInterval(() => cmd('/state'), 500);
  cmd('/state');
</script>
</body>
</html>
)rawliteral";

// ---------- TM1637 ----------
// Teli "1" = e lëmë të lirë (rezistenca e ekranit e tërheq lart), "0" = e tërheqim në GND.
void tmTel(uint8_t pin, bool vlera) {
  if (vlera) pinMode(pin, INPUT_PULLUP);
  else { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
  delayMicroseconds(5);
}

void tmFillo() { tmTel(TM_DIO, 1); tmTel(TM_CLK, 1); tmTel(TM_DIO, 0); }
void tmMbaro() { tmTel(TM_CLK, 0); tmTel(TM_DIO, 0); tmTel(TM_CLK, 1); tmTel(TM_DIO, 1); }

// Dërgon një byte (biti më i vogël i pari). Kthen true nëse ekrani tha "e mora" (ACK).
bool tmByte(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    tmTel(TM_CLK, 0);
    tmTel(TM_DIO, b & 1);
    tmTel(TM_CLK, 1);
    b >>= 1;
  }
  tmTel(TM_CLK, 0);
  tmTel(TM_DIO, 1);
  tmTel(TM_CLK, 1);
  bool ack = digitalRead(TM_DIO) == LOW;
  tmTel(TM_CLK, 0);
  return ack;
}

// Shkruan 4 shifrat (segmentet) në ekran
bool tmShfaq(const uint8_t seg[4]) {
  tmFillo(); bool ok = tmByte(0x40); tmMbaro();          // shkruaj me adresa që rriten vetë
  tmFillo(); ok &= tmByte(0xC0);                         // fillo nga shifra e parë
  for (int i = 0; i < 4; i++) ok &= tmByte(seg[i]);
  tmMbaro();
  tmFillo(); ok &= tmByte(0x88 | TM_NDRICIMI); tmMbaro(); // ndize ekranin
  return ok;
}

const uint8_t SHIFRAT[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
#define GRADE 0x63   // shenja ° (katrori i vogël lart)

// Këndi i servos, p.sh. " 90°" ose "180°"
void shfaqKendin(int angle) {
  uint8_t seg[4];
  seg[0] = angle >= 100 ? SHIFRAT[angle / 100] : 0;
  seg[1] = angle >= 10 ? SHIFRAT[(angle / 10) % 10] : 0;
  seg[2] = SHIFRAT[angle % 10];
  seg[3] = GRADE;
  tmShfaq(seg);
}
// ----------------------------

void moveTo(int angle) {
  currentAngle = angle;
  servo.write(angle);
  shfaqKendin(angle);
  Serial.printf("STATE angle=%d sweep=%d\n", currentAngle, sweeping ? 1 : 0);
}

void sendState() {
  String json = String("{\"angle\":") + currentAngle +
                ",\"sweeping\":" + (sweeping ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void forgetWifi() {
  Serial.println(">>> Po harroj WiFi-n dhe po rindez...");
  wm.resetSettings();
  delay(500);
  ESP.restart();
}

void handleCommand(String cmd) {
  cmd.trim();
  cmd.toLowerCase();
  if (cmd == "start") {
    sweeping = true;
    Serial.println(">>> START");
  } else if (cmd == "stop") {
    sweeping = false;
    Serial.println(">>> STOP");
  } else if (cmd == "state") {
    Serial.printf("STATE angle=%d sweep=%d IP=%s\n", currentAngle, sweeping ? 1 : 0,
                  WiFi.localIP().toString().c_str());
  } else if (cmd == "resetwifi") {
    forgetWifi();
  } else if (cmd.length() > 0 && isDigit(cmd[0])) {
    int angle = cmd.toInt();
    if (angle >= 0 && angle <= 180) {
      sweeping = false;   // kur jep kënd me dorë, ndalon lëvizja automatike
      moveTo(angle);
    } else {
      Serial.println("Këndi duhet të jetë nga 0 deri 180");
    }
  } else if (cmd.length() > 0) {
    Serial.println("Komandat: start, stop, state, resetwifi, ose një numër 0-180");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);                 // mos prit gjatë kur lexon një komandë
  pinMode(BOOT_PIN, INPUT_PULLUP);

  // Test i ekranit: ndez të gjitha segmentet ("8.8.8.8.") për 1 sekondë
  const uint8_t teGjitha[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  Serial.printf("Ekrani TM1637 (DIO=%d, CLK=%d): %s\n", TM_DIO, TM_CLK,
                tmShfaq(teGjitha) ? "OK" : "NUK PERGJIGJET - kontrollo telat");
  delay(1000);

  servo.setPeriodHertz(50);             // servot standarde punojnë me 50 Hz
  servo.attach(SERVO_PIN, 500, 2400);    // gjatësia e pulsit për 0° dhe 180°
  moveTo(90);                            // fillon në mes

  // Provon WiFi-n e ruajtur; nëse s'e gjen, hap "servoLocal" dhe pret konfigurimin
  WiFi.setHostname(HOSTNAME);
  Serial.println("Po lidhem me WiFi. Nese s'ka WiFi te ruajtur, lidhu me '" SETUP_AP_SSID "'");
  wm.setTitle("Servo - Zgjidh WiFi-n");
  if (!wm.autoConnect(SETUP_AP_SSID, SETUP_AP_PASS)) {
    Serial.println("WiFi deshtoi, po rindez...");
    ESP.restart();
  }

  Serial.print("WiFi OK: ");
  Serial.print(WiFi.SSID());
  Serial.print("  IP: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("Hap ne browser: http://servo.local");
  }

  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/state", sendState);
  server.on("/start", []() { handleCommand("start"); sendState(); });
  server.on("/stop",  []() { handleCommand("stop");  sendState(); });
  server.on("/set", []() {
    if (server.hasArg("angle")) handleCommand(server.arg("angle"));
    sendState();
  });
  server.begin();

  Serial.println("Gati. Shkruaj: start, stop, state, resetwifi, ose një numër 0-180");
}

void loop() {
  server.handleClient();

  if (Serial.available()) {
    handleCommand(Serial.readStringUntil('\n'));
  }

  // BOOT i mbajtur 3 sekonda -> harro WiFi-n
  if (digitalRead(BOOT_PIN) == LOW) {
    if (bootPressedMs == 0) bootPressedMs = millis();
    if (millis() - bootPressedMs > 3000) forgetWifi();
  } else {
    bootPressedMs = 0;
  }

  if (sweeping && millis() - lastMoveMs > 1500) {
    lastMoveMs = millis();
    moveTo(steps[stepIndex]);
    stepIndex = (stepIndex + 1) % 4;
  }
}
