// ============================================================
//  SMART MEDICINE DISPENSER — ESP32
//  Components:
//    - DS1302 RTC (CLK=18, DAT=19, RST=5)
//    - SSD1306 OLED I2C (SDA=21, SCL=22)
//    - SG90 Servo (Signal=13)
//    - FC-51 IR Sensor (OUT=27)
//    - Passive Buzzer (+=25)
//    - 3-pin Pulse Sensor (Signal=34)
//    - NEO-6M GPS (TX=16, RX=17)
//  Web Interface: Connect to WiFi "MediDispenser"
//  Open browser: 192.168.4.1
// ============================================================

// ── LIBRARIES ───────────────────────────────────────────────
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>

// ── PIN DEFINITIONS ─────────────────────────────────────────
#define RTC_CLK     18
#define RTC_DAT     19
#define RTC_RST     5
#define SERVO_PIN   13
#define IR_PIN      27
#define BUZZER_PIN  25
#define PULSE_PIN   34   // Analog input
#define GPS_RX      16   // GPS TX → ESP32 RX2
#define GPS_TX      17   // GPS RX → ESP32 TX2

// ── OLED ────────────────────────────────────────────────────
#define OLED_WIDTH  128
#define OLED_HEIGHT 64
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// ── RTC DS1302 ──────────────────────────────────────────────
ThreeWire myWire(RTC_DAT, RTC_CLK, RTC_RST);
RtcDS1302<ThreeWire> Rtc(myWire);

// ── SERVO ───────────────────────────────────────────────────
Servo dispenseServo;

// ── GPS ─────────────────────────────────────────────────────
TinyGPSPlus gps;
HardwareSerial gpsSerial(2); // UART2
double gpsLat = 0.0, gpsLng = 0.0;
bool gpsFixed = false;

// ── WiFi AP ─────────────────────────────────────────────────
const char* ssid     = "MediDispenser";
const char* password = "medicine123";
WebServer server(80);

// ── SCHEDULE ────────────────────────────────────────────────
// 3 doses: Morning, Lunch, Dinner
int schedHour[3]   = {8,  13, 20};
int schedMinute[3] = {0,  0,  0};
const char* doseNames[] = {"Morning", "Lunch", "Dinner"};

// ── STATUS ──────────────────────────────────────────────────
String doseStatus[3]  = {"Pending", "Pending", "Pending"};
int    dosePulse[3]   = {0, 0, 0};
bool   doseDispensed[3] = {false, false, false};
int    lastDay = -1;

// ── LOG ─────────────────────────────────────────────────────
String logEntries[30];
int    logCount = 0;

// ── PULSE ───────────────────────────────────────────────────
int currentPulse = 0;

// ── SERVO POSITIONS (carousel) ──────────────────────────────
// 3 compartments — each 120 degrees apart
int servoPos[5] = {0, 60, 120,180,0};
int currentServoPos = 0;

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  // ── Pins
  pinMode(IR_PIN,     INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(PULSE_PIN,  INPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // ── Servo
  dispenseServo.attach(SERVO_PIN);
  dispenseServo.write(0);
  delay(500);

  // ── I2C
  Wire.begin(21, 22);

  // ── OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED failed!");
  } else {
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(2);
    display.setCursor(10, 20);
    display.print("MediBot");
    display.setTextSize(1);
    display.setCursor(15, 45);
    display.print("Starting...");
    display.display();
  }

  // ── RTC DS1302
  Rtc.Begin();
  RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);

  if (!Rtc.IsDateTimeValid()) {
    Serial.println("RTC invalid — setting time from compile time");
    Rtc.SetDateTime(compiled);
  }
  if (Rtc.GetIsWriteProtected()) {
    Rtc.SetIsWriteProtected(false);
  }
  if (!Rtc.GetIsRunning()) {
    Rtc.SetIsRunning(true);
  }

  // ── GPS Serial
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("GPS Serial started");

  // ── WiFi AP
  WiFi.softAP(ssid, password);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  // ── Web routes
  server.on("/",          handleRoot);
  server.on("/save",      handleSave);
  server.on("/logs",      handleLogs);
  server.on("/settime",   handleSetTime);
  server.begin();

  // ── Startup melody
  playStartup();

  // ── OLED ready
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("WiFi: MediDispenser");
  display.setCursor(0, 12);
  display.print("IP: 192.168.4.1");
  display.display();
  delay(2000);

  Serial.println("Setup complete!");
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  server.handleClient();

  // ── Read GPS
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }
  if (gps.location.isValid()) {
    gpsLat   = gps.location.lat();
    gpsLng   = gps.location.lng();
    gpsFixed = true;
  }

  // ── Get current time from RTC
  RtcDateTime now = Rtc.GetDateTime();
  if (!now.IsValid()) {
    Serial.println("RTC read error");
    delay(1000);
    return;
  }

  int curHour   = now.Hour();
  int curMinute = now.Minute();
  int curDay    = now.Day();

  // ── Reset at midnight
  if (curDay != lastDay) {
    for (int i = 0; i < 3; i++) {
      doseDispensed[i] = false;
      doseStatus[i]    = "Pending";
      dosePulse[i]     = 0;
    }
    lastDay = curDay;
    addLog("New day — schedule reset");
  }

  // ── Check each scheduled dose
  for (int i = 0; i < 3; i++) {
    if (!doseDispensed[i]
        && curHour   == schedHour[i]
        && curMinute == schedMinute[i]) {
      triggerDose(i, now);
      doseDispensed[i] = true;
    }
  }

  // ── Read pulse continuously
  readPulse();

  // ── Update OLED
  updateOLED(now);

  delay(500);
}

// ============================================================
// TRIGGER DOSE
// ============================================================
void triggerDose(int idx, RtcDateTime now) {
  Serial.print("Dispensing: ");
  Serial.println(doseNames[idx]);

  // 1. Chime alarm
  playChime();

  // 2. OLED — medicine time
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print("MEDICINE");
  display.setCursor(0, 20);
  display.print("TIME!");
  display.setTextSize(1);
  display.setCursor(0, 45);
  display.print(doseNames[idx]);
  display.print(" dose");
  display.display();

  // 3. Rotate carousel to correct position
  rotateTo(servoPos[idx]);
  delay(800);

  // 4. Open gate — rotate 30 more degrees to drop pill
  dispenseServo.write(servoPos[idx] + 30);
  delay(800);
  dispenseServo.write(servoPos[idx]);
  delay(500);

  // 5. Wait for pill pickup — 40 seconds window
  playTwoBeeps(); // "Please pick up medicine"

  bool taken    = false;
  bool irWasBlocked = false;
  long waitStart = millis();

  while (millis() - waitStart < 40000) {
    int irVal = digitalRead(IR_PIN);

    // IR LOW = object detected (pill in tray)
    if (irVal == LOW) irWasBlocked = true;

    // Pill was there and now removed = TAKEN
    if (irWasBlocked && irVal == HIGH) {
      taken = true;
      break;
    }

    server.handleClient();
    delay(100);
  }

  if (taken) {
    // 6. Pill taken — now measure pulse
    doseStatus[idx] = "Taken";
    playPulseMeasureBeep(); // "Place finger on sensor"

    // OLED
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Medicine TAKEN!");
    display.setCursor(0, 16);
    display.print("Place finger on");
    display.setCursor(0, 26);
    display.print("pulse sensor...");
    display.display();

    // Measure pulse for 15 seconds
    int pulse = measurePulse(15000);
    dosePulse[idx] = pulse;

    // Done beep
    playDoneBeep();

    // OLED update
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Pulse measured!");
    display.setTextSize(2);
    display.setCursor(20, 20);
    display.print(pulse);
    display.print(" bpm");
    display.display();
    delay(3000);

    // Abnormal pulse alert
    if (pulse > 0 && (pulse < 50 || pulse > 120)) {
      playAbnormalAlert();
      addLog("ALERT! Abnormal HR: " + String(pulse) + " bpm after " + String(doseNames[idx]));
    }

    addLog(String(doseNames[idx]) + " TAKEN — HR: " + String(pulse) + " bpm");
    Serial.println("Dose taken!");

  } else {
    // Missed
    doseStatus[idx] = "Missed";
    playMissedAlert();

    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(0, 10);
    display.print("MISSED!");
    display.setTextSize(1);
    display.setCursor(0, 40);
    display.print(doseNames[idx]);
    display.print(" dose not taken");
    display.display();
    delay(3000);

    addLog(String(doseNames[idx]) + " MISSED!");
    Serial.println("Dose missed!");
  }
}

// ============================================================
// SERVO CAROUSEL ROTATION
// ============================================================
void rotateTo(int targetAngle) {
  // Smooth rotation
  int step = (targetAngle > currentServoPos) ? 2 : -2;
  while (currentServoPos != targetAngle) {
    currentServoPos += step;
    if (abs(currentServoPos - targetAngle) < 2) currentServoPos = targetAngle;
    dispenseServo.write(currentServoPos);
    delay(15);
  }
}

// ============================================================
// PULSE READING
// ============================================================
void readPulse() {
  // Simple analog read — basic BPM estimation
  int raw = analogRead(PULSE_PIN);
  // Map raw to approximate BPM range for display
  // Actual BPM calculation needs peak detection — simplified here
  if (raw > 2000) {
    currentPulse = map(raw, 2000, 4095, 60, 100);
  } else {
    currentPulse = 0; // No finger detected
  }
}

int measurePulse(long duration) {
  // Measure pulse over given duration using peak detection
  long start   = millis();
  int  peaks   = 0;
  int  lastVal = 0;
  bool rising  = false;
  int  threshold = 2500;
  long lastPeak  = 0;
  int  bpmSum    = 0;
  int  bpmCount  = 0;

  while (millis() - start < duration) {
    int val = analogRead(PULSE_PIN);

    if (!rising && val > threshold) {
      rising = true;
    }
    if (rising && val < threshold) {
      rising = false;
      long now = millis();
      if (lastPeak > 0) {
        long interval = now - lastPeak;
        if (interval > 300 && interval < 2000) {
          int bpm = 60000 / interval;
          bpmSum   += bpm;
          bpmCount++;
        }
      }
      lastPeak = now;
      peaks++;
    }

    server.handleClient();
    delay(10);
  }

  if (bpmCount > 0) return bpmSum / bpmCount;
  return 0;
}

// ============================================================
// OLED UPDATE
// ============================================================
void updateOLED(RtcDateTime now) {
  display.clearDisplay();

  // Line 1: Time
  display.setTextSize(2);
  display.setCursor(0, 0);
  if (now.Hour()   < 10) display.print("0");
  display.print(now.Hour());
  display.print(":");
  if (now.Minute() < 10) display.print("0");
  display.print(now.Minute());

  // Line 2: Pulse
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.print("HR: ");
  if (currentPulse > 0) {
    display.print(currentPulse);
    display.print(" bpm");
  } else {
    display.print("--");
  }

  // Line 3: Next dose
  display.setCursor(0, 32);
  display.print("Next: ");
  int nowMins = now.Hour() * 60 + now.Minute();
  bool found = false;
  for (int i = 0; i < 3; i++) {
    int doseMins = schedHour[i] * 60 + schedMinute[i];
    if (doseMins > nowMins && !doseDispensed[i]) {
      display.print(doseNames[i]);
      display.print(" ");
      if (schedHour[i]   < 10) display.print("0");
      display.print(schedHour[i]);
      display.print(":");
      if (schedMinute[i] < 10) display.print("0");
      display.print(schedMinute[i]);
      found = true;
      break;
    }
  }
  if (!found) display.print("Tomorrow");

  // Line 4: GPS
  display.setCursor(0, 44);
  display.print("GPS: ");
  display.print(gpsFixed ? "Fixed OK" : "Searching..");

  // Line 5: WiFi reminder
  display.setCursor(0, 56);
  display.print("192.168.4.1");

  display.display();
}

// ============================================================
// LOG
// ============================================================
void addLog(String msg) {
  String entry = "[" + String(millis()/1000) + "s] " + msg;
  if (logCount < 30) {
    logEntries[logCount++] = entry;
  } else {
    for (int i = 0; i < 29; i++) logEntries[i] = logEntries[i+1];
    logEntries[29] = entry;
  }
  Serial.println(entry);
}

// ============================================================
// BUZZER MELODIES
// ============================================================
void playStartup() {
  // Happy startup chime
  int notes[] = {262, 330, 392, 523, 392, 523};
  int dur[]   = {150, 150, 150, 300, 150, 400};
  for (int i = 0; i < 6; i++) {
    tone(BUZZER_PIN, notes[i], dur[i]);
    delay(dur[i] + 50);
  }
  noTone(BUZZER_PIN);
}

void playChime() {
  // Soothing 3-tone chime — repeated 3x
  for (int r = 0; r < 3; r++) {
    tone(BUZZER_PIN, 523, 200); delay(250);
    tone(BUZZER_PIN, 659, 200); delay(250);
    tone(BUZZER_PIN, 784, 300); delay(600);
  }
  noTone(BUZZER_PIN);
}

void playTwoBeeps() {
  // Please pick up medicine
  tone(BUZZER_PIN, 880, 150); delay(200);
  tone(BUZZER_PIN, 880, 150); delay(200);
  noTone(BUZZER_PIN);
}

void playPulseMeasureBeep() {
  // Please place finger
  tone(BUZZER_PIN, 660, 500);
  delay(600);
  noTone(BUZZER_PIN);
}

void playDoneBeep() {
  // Measurement done
  tone(BUZZER_PIN, 784, 100); delay(130);
  tone(BUZZER_PIN, 988, 300); delay(350);
  noTone(BUZZER_PIN);
}

void playMissedAlert() {
  // Urgent missed dose
  for (int i = 0; i < 8; i++) {
    tone(BUZZER_PIN, 880, 150);
    delay(200);
  }
  noTone(BUZZER_PIN);
}

void playAbnormalAlert() {
  // Abnormal heart rate
  for (int i = 0; i < 5; i++) {
    tone(BUZZER_PIN, 1200, 200); delay(100);
    tone(BUZZER_PIN, 600,  200); delay(100);
  }
  noTone(BUZZER_PIN);
}

// ============================================================
// WEB SERVER — MAIN PAGE
// ============================================================
void handleRoot() {
  RtcDateTime now = Rtc.GetDateTime();

  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='30'>"; // auto refresh every 30s
  html += "<title>MediDispenser</title>";
  html += "<style>";
  html += "body{font-family:sans-serif;background:#0d1117;color:#e6edf3;padding:15px;max-width:480px;margin:auto}";
  html += "h1{color:#00d4aa;font-size:1.4rem;margin-bottom:5px}";
  html += "h2{color:#f59e0b;font-size:1rem;margin:18px 0 8px}";
  html += ".card{background:#1f2937;border-radius:10px;padding:14px;margin:10px 0}";
  html += ".stat{color:#00d4aa;font-size:1.6rem;font-weight:bold}";
  html += ".taken{color:#22c55e;font-weight:bold}";
  html += ".missed{color:#ef4444;font-weight:bold}";
  html += ".pending{color:#f59e0b}";
  html += "input[type=number]{background:#374151;border:1px solid #4b5563;color:#e6edf3;padding:6px;border-radius:5px;width:60px;font-size:1rem}";
  html += "label{display:block;margin:8px 0;font-size:0.9rem}";
  html += ".btn{background:#00d4aa;color:#000;border:none;padding:10px 0;border-radius:8px;font-weight:bold;cursor:pointer;width:100%;font-size:1rem;margin-top:8px}";
  html += ".btn2{background:#6366f1;color:#fff;border:none;padding:10px 0;border-radius:8px;font-weight:bold;cursor:pointer;width:100%;font-size:0.9rem;margin-top:6px}";
  html += ".gps-box{background:#0d2a1f;border:1px solid #00d4aa44;border-radius:8px;padding:10px;margin-top:10px}";
  html += ".alert-box{background:#2a0d0d;border:1px solid #ef444444;border-radius:8px;padding:10px;margin-top:10px;color:#ef4444}";
  html += "</style></head><body>";

  // Header
  html += "<h1>💊 MediDispenser</h1>";

  // Time + Date card
  html += "<div class='card'>";
  html += "<b>🕐 Time: </b><span class='stat'>";
  if (now.Hour()   < 10) html += "0";
  html += String(now.Hour()) + ":";
  if (now.Minute() < 10) html += "0";
  html += String(now.Minute());
  html += "</span><br><br>";
  html += "<b>📅 Date: </b>" + String(now.Day()) + "/" + String(now.Month()) + "/" + String(now.Year());
  html += "</div>";

  // Dose status cards
  html += "<h2>📋 Today's Doses</h2>";
  for (int i = 0; i < 3; i++) {
    html += "<div class='card'>";
    html += "<b>";
    html += (i==0 ? "🌅 " : i==1 ? "☀️ " : "🌙 ");
    html += String(doseNames[i]) + "</b> — ";
    if (schedHour[i] < 10) html += "0";
    html += String(schedHour[i]) + ":";
    if (schedMinute[i] < 10) html += "0";
    html += String(schedMinute[i]);
    html += "<br><span class='";
    if      (doseStatus[i] == "Taken")   html += "taken'>✅ TAKEN";
    else if (doseStatus[i] == "Missed")  html += "missed'>❌ MISSED";
    else                                  html += "pending'>⏳ Pending";
    html += "</span>";
    if (dosePulse[i] > 0) {
      html += " &nbsp; ❤️ <b>" + String(dosePulse[i]) + " bpm</b>";
      // Abnormal highlight
      if (dosePulse[i] < 50 || dosePulse[i] > 120) {
        html += " <span style='color:#ef4444'>⚠️ Abnormal!</span>";
      }
    }
    html += "</div>";
  }

  // Pulse reading
  html += "<div class='card'>";
  html += "❤️ <b>Current Pulse:</b> ";
  if (currentPulse > 0) {
    html += "<span class='stat'>" + String(currentPulse) + " bpm</span>";
    if (currentPulse < 50 || currentPulse > 120) {
      html += "<div class='alert-box'>⚠️ Abnormal Heart Rate Detected!</div>";
    }
  } else {
    html += "<span style='color:#6b7280'>No finger detected</span>";
  }
  html += "</div>";

  // GPS location
  html += "<h2>📍 Location</h2>";
  html += "<div class='gps-box'>";
  if (gpsFixed) {
    html += "✅ <b>GPS Fixed</b><br>";
    html += "Lat: <b>" + String(gpsLat, 6) + "</b><br>";
    html += "Lng: <b>" + String(gpsLng, 6) + "</b><br><br>";
    html += "<a href='https://maps.google.com/?q=" + String(gpsLat,6) + "," + String(gpsLng,6) + "' target='_blank' style='color:#00d4aa'>🗺️ Open in Google Maps</a>";
  } else {
    html += "🔍 Searching for GPS signal...<br>";
    html += "<small style='color:#6b7280'>Place near window for better signal</small>";
  }
  html += "</div>";

  // Schedule form
  html += "<h2>⏰ Set Schedule</h2>";
  html += "<form action='/save'><div class='card'>";
  html += "<label>🌅 Morning: <input name='mh' type='number' min='0' max='23' value='" + String(schedHour[0]) + "'> : <input name='mm' type='number' min='0' max='59' value='" + String(schedMinute[0]) + "'></label>";
  html += "<label>☀️ Lunch &nbsp;: <input name='lh' type='number' min='0' max='23' value='" + String(schedHour[1]) + "'> : <input name='lm' type='number' min='0' max='59' value='" + String(schedMinute[1]) + "'></label>";
  html += "<label>🌙 Dinner &nbsp;: <input name='dh' type='number' min='0' max='23' value='" + String(schedHour[2]) + "'> : <input name='dm' type='number' min='0' max='59' value='" + String(schedMinute[2]) + "'></label>";
  html += "<input class='btn' type='submit' value='💾 Save Schedule'>";
  html += "</div></form>";

  // Set time form
  html += "<h2>🕐 Set RTC Time</h2>";
  html += "<form action='/settime'><div class='card'>";
  html += "<label>Hour (0-23): <input name='th' type='number' min='0' max='23' value='" + String(now.Hour()) + "'></label>";
  html += "<label>Minute (0-59): <input name='tm' type='number' min='0' max='59' value='" + String(now.Minute()) + "'></label>";
  html += "<label>Day: <input name='td' type='number' min='1' max='31' value='" + String(now.Day()) + "'></label>";
  html += "<label>Month: <input name='tmo' type='number' min='1' max='12' value='" + String(now.Month()) + "'></label>";
  html += "<label>Year: <input name='ty' type='number' min='2024' max='2099' value='" + String(now.Year()) + "'></label>";
  html += "<input class='btn' type='submit' value='🕐 Set Time'>";
  html += "</div></form>";

  // Logs button
  html += "<br><a href='/logs'><button class='btn2'>📋 View Full Log</button></a>";
  html += "<br><br></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

// ============================================================
// SAVE SCHEDULE
// ============================================================
void handleSave() {
  if (server.hasArg("mh"))  schedHour[0]   = server.arg("mh").toInt();
  if (server.hasArg("mm"))  schedMinute[0] = server.arg("mm").toInt();
  if (server.hasArg("lh"))  schedHour[1]   = server.arg("lh").toInt();
  if (server.hasArg("lm"))  schedMinute[1] = server.arg("lm").toInt();
  if (server.hasArg("dh"))  schedHour[2]   = server.arg("dh").toInt();
  if (server.hasArg("dm"))  schedMinute[2] = server.arg("dm").toInt();

  addLog("Schedule updated: M=" + String(schedHour[0]) + ":" + String(schedMinute[0]) +
         " L=" + String(schedHour[1]) + ":" + String(schedMinute[1]) +
         " D=" + String(schedHour[2]) + ":" + String(schedMinute[2]));

  server.sendHeader("Location", "/");
  server.send(302);
}

// ============================================================
// SET RTC TIME
// ============================================================
void handleSetTime() {
  int h   = server.hasArg("th")  ? server.arg("th").toInt()  : 0;
  int m   = server.hasArg("tm")  ? server.arg("tm").toInt()  : 0;
  int d   = server.hasArg("td")  ? server.arg("td").toInt()  : 1;
  int mo  = server.hasArg("tmo") ? server.arg("tmo").toInt() : 1;
  int y   = server.hasArg("ty")  ? server.arg("ty").toInt()  : 2024;

  RtcDateTime newTime(y, mo, d, h, m, 0);
  Rtc.SetDateTime(newTime);

  addLog("RTC time set to: " + String(d) + "/" + String(mo) + "/" + String(y) +
         " " + String(h) + ":" + String(m));

  server.sendHeader("Location", "/");
  server.send(302);
}

// ============================================================
// LOGS PAGE
// ============================================================
void handleLogs() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Logs</title>";
  html += "<style>body{font-family:sans-serif;background:#0d1117;color:#e6edf3;padding:15px;max-width:480px;margin:auto}";
  html += "h1{color:#00d4aa} .log{background:#1f2937;padding:8px 12px;margin:5px 0;border-radius:6px;font-family:monospace;font-size:0.82rem}";
  html += ".taken{border-left:3px solid #22c55e} .missed{border-left:3px solid #ef4444} .info{border-left:3px solid #6366f1}";
  html += "a{color:#6366f1}</style></head><body>";
  html += "<h1>📋 Medicine Log</h1><a href='/'>← Back</a><br><br>";

  if (logCount == 0) {
    html += "<div class='log info'>No logs yet.</div>";
  } else {
    for (int i = logCount - 1; i >= 0; i--) {
      String cls = "info";
      if (logEntries[i].indexOf("TAKEN")  >= 0) cls = "taken";
      if (logEntries[i].indexOf("MISSED") >= 0) cls = "missed";
      html += "<div class='log " + cls + "'>" + logEntries[i] + "</div>";
    }
  }

  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}
