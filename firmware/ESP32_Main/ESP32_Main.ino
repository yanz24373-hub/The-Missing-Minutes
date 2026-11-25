// Main robot: task timing, display, controls and wireless audio.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Servo.h>

#include "wifi_secrets.h"
const char* TIMEZONE = "GMT0BST,M3.5.0/1,M10.5.0/2";

#define TFT_CS   27
#define TFT_DC   26
#define TFT_RST  25
#define TFT_MOSI 23
#define TFT_SCLK 18
#define ENC_CLK 32
#define ENC_DT  33
#define ENC_SW  13
#define TOUCH_PIN 14
#define SERVO_PIN 16
#define LED_PIN   17
#define LED_COUNT 33

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
Servo headServo;
WiFiUDP trayUdp;

#define BG      ST77XX_BLACK
#define WHITE_C ST77XX_WHITE
#define MINT    0xA77A
#define GOLD    0xF6AC
#define DIM     0x7BEF
#define BLUE    0x249F
#define PURPLE  0xA2BE
#define GREEN   0x3666
#define GREY    0x9CF3

const uint16_t TRAY_PORT = 4210;
const uint16_t AUDIO_COMMAND_PORT = 4211;
const unsigned long AUDIO_RETRY_MS = 90;
const unsigned long AUDIO_COMMAND_TTL_MS = 10000;  
const int AUDIO_SEND_COPIES = 3;

const uint8_t AUDIO_WARNING_TRACK = 1;
const uint8_t AUDIO_OVERDUE_TRACK = 2;
const uint8_t AUDIO_COMPLETE_TRACK = 3;
const unsigned long TRAY_TIMEOUT = 7000;
const unsigned long LED_REMINDER_MS = 10000;
const unsigned long HEAD_REMINDER_MS = 20000;
const int SERVO_CENTER = 90;
const int SERVO_RANGE = 30; 
const unsigned long SERVO_STEP_MS = 25;

const float WEATHER_LAT = 51.55f;
const float WEATHER_LON = -0.22f;

struct TaskStep {
  const char* line1;
  const char* line2;
  uint16_t weight;
};

const TaskStep TASKS[] = {
  {"GET", "DRESSED", 3},
  {"PACK", "BAG", 2},
  {"GET", "READY", 2}
};
const int TASK_COUNT = sizeof(TASKS) / sizeof(TASKS[0]);

enum RobotMode { IDLE, DIZZY, SET_HOUR, SET_MINUTE,
                 TASK_RUNNING, FINAL_KEY, CONFIRM_UNKNOWN, COMPLETED };
enum KeyState { KEY_UNKNOWN, KEY_IN, KEY_NOT_IN };
enum IdlePage { EYES_PAGE, FISH_PAGE, WEATHER_PAGE };
enum FishPhase { FISH_SWIM, FISH_SCATTER, FISH_HIDE, FISH_RETURN };
enum LedMode { LED_OFF, LED_FLOW, LED_FLASH };

RobotMode mode = IDLE;
KeyState keyState = KEY_UNKNOWN;
IdlePage idlePage = EYES_PAGE;
FishPhase fishPhase = FISH_SWIM;
LedMode ledMode = LED_OFF;

int currentStep = 0;
int targetHour = 9;
int targetMinute = 0;
time_t departureEpoch = 0;
bool departureAlerted = false;
unsigned long taskStartedAt = 0;
unsigned long currentTaskBudgetSec = 0;

bool trayPresent = false;
bool trayEverReceived = false;
bool keySeenThisSession = false;
bool manualKeyConfirmed = false;
unsigned long lastTrayPacket = 0;
bool keyDirty = false;

IPAddress lastTraySenderIP(0, 0, 0, 0);
bool trayAddressKnown = false;

bool warningSoundPlayed = false;
bool overdueSoundPlayed = false;
uint32_t nextAudioCommandId = 1;
uint32_t queuedAudioCommandId = 0;
uint8_t queuedAudioTrack = 0;
uint8_t audioCopiesSent = 0;
unsigned long lastAudioSendAt = 0;
unsigned long audioQueuedAt = 0;
unsigned long lastTrayDebugAt = 0;
unsigned long lastRawUdpAt = 0;

bool wifiStarted = false;
bool clockReported = false;
bool weatherOK = false;
bool weatherAttempted = false;
unsigned long wifiRetryAt = 0;
unsigned long weatherLastAttempt = 0;
float weatherTemp = 0;
int weatherCode = 0;

int lastEncoderCLK = HIGH;
unsigned long lastEncoderStep = 0;
int buttonRaw = HIGH;
int buttonStable = HIGH;
unsigned long buttonChangedAt = 0;
unsigned long buttonDownAt = 0;
int touchRaw = LOW;
int touchStable = LOW;
unsigned long touchChangedAt = 0;
unsigned long lastTouchEvent = 0;

bool eyesClosed = false;
unsigned long lastBlinkAt = 0;
unsigned long eyeCloseAt = 0;
unsigned long dizzyAt = 0;
unsigned long lastInteractionAt = 0;
unsigned long lastIdlePageAt = 0;
unsigned long lastLedReminder = 0;
unsigned long lastHeadReminder = 0;
unsigned long lastTaskTimeUpdate = 0;

bool servoMoving = false;
int servoPosition = SERVO_CENTER;
int servoTarget = SERVO_CENTER;
int servoPhase = 0;
unsigned long servoLastStep = 0;
unsigned long servoHoldAt = 0;

int ledFrame = 0;
int ledPhase = 0;
unsigned long ledChangedAt = 0;

int fishX[3] = {30, 115, 74};
const int fishY[3] = {43, 64, 83};
int fishDir[3] = {1, -1, 1};
const int fishGoal[3] = {36, 116, 79};
unsigned long fishPhaseAt = 0;
unsigned long fishFrameAt = 0;
unsigned long fishHoldUntil = 0;

unsigned long weatherAnimAt = 0;
int weatherFrame = 0;

String cachedTaskTime = "";
String cachedKeyState = "";

bool realClock() { return time(nullptr) > 1700000000; }
bool trayOnline() {
  return WiFi.status() == WL_CONNECTED && trayEverReceived &&
         millis() - lastTrayPacket < TRAY_TIMEOUT;
}

void centerText(const char* s, int y, int sz, uint16_t color,
                int x0 = 0, int width = 160) {
  tft.setTextSize(sz);
  tft.setTextColor(color);
  int x = x0 + (width - (int)strlen(s) * 6 * sz) / 2;
  if (x < x0) x = x0;
  tft.setCursor(x, y);
  tft.print(s);
}

void drawClockSmall() {
  char b[12] = "--:--";
  if (realClock()) {
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    snprintf(b, sizeof(b), "%02d:%02d", local.tm_hour, local.tm_min);
  }
  tft.fillRect(5, 2, 45, 10, BG);
  tft.setTextSize(1);
  tft.setTextColor(DIM);
  tft.setCursor(5, 3);
  tft.print(b);
}

void swingHead() {
  if (servoMoving) return;
  servoMoving = true;
  servoPhase = 0;
  servoTarget = SERVO_CENTER - SERVO_RANGE;
  servoLastStep = millis();
}
void centerHead() {
  servoMoving = true;
  servoPhase = 4;
  servoTarget = SERVO_CENTER;
  servoLastStep = millis();
}
void updateServo() {
  if (!servoMoving || millis() - servoLastStep < SERVO_STEP_MS) return;
  servoLastStep = millis();
  if (servoPhase == 0 || servoPhase == 2 || servoPhase == 4) {
    if (servoPosition < servoTarget) servoPosition++;
    else if (servoPosition > servoTarget) servoPosition--;
    headServo.write(servoPosition);
    if (servoPosition == servoTarget) {
      servoHoldAt = millis();
      if (servoPhase == 0) servoPhase = 1;
      else if (servoPhase == 2) servoPhase = 3;
      else servoMoving = false;
    }
  } else if (servoPhase == 1 && millis() - servoHoldAt >= 150) {
    servoTarget = SERVO_CENTER + SERVO_RANGE;
    servoPhase = 2;
  } else if (servoPhase == 3 && millis() - servoHoldAt >= 150) {
    servoTarget = SERVO_CENTER;
    servoPhase = 4;
  }
}

void ledClear() { strip.clear(); strip.show(); }
void ledStop() { ledMode = LED_OFF; ledClear(); }
void ledAll() { strip.fill(strip.Color(90, 125, 100)); strip.show(); }
void tripleFlash() {
  ledMode = LED_FLASH;
  ledPhase = 0;
  ledChangedAt = millis();
  ledAll();
}
void paintFlow(int frame) {
  const uint8_t g[5] = {140, 90, 48, 20, 6};
  const uint8_t b[5] = {100, 60, 30, 12, 4};
  strip.clear();
  for (int i = 0; i < 5; i++) {
    int p = (frame - i + LED_COUNT * 3) % LED_COUNT;
    strip.setPixelColor(p, strip.Color(0, g[i], b[i]));
  }
  strip.show();
}
void startFlow() {
  if (ledMode != LED_OFF) return;
  ledMode = LED_FLOW;
  ledFrame = 0;
  ledChangedAt = millis();
  paintFlow(0);
}
void updateLed() {
  unsigned long now = millis();
  if (ledMode == LED_FLASH) {
    unsigned long interval = (ledPhase % 2 == 0) ? 150UL : 120UL;
    if (now - ledChangedAt >= interval) {
      ledChangedAt = now;
      ledPhase++;
      if (ledPhase >= 6) ledStop();
      else if (ledPhase % 2 == 0) ledAll();
      else ledClear();
    }
  } else if (ledMode == LED_FLOW && now - ledChangedAt >= 55) {
    ledChangedAt = now;
    ledFrame++;
    if (ledFrame >= LED_COUNT * 2) ledStop();
    else paintFlow(ledFrame);
  }
}

void drawEye(int cx, int cy) {
  for (int dx = -19; dx <= 19; dx++) {
    int yy = cy - 15 + (15 * dx * dx) / 361;
    for (int d = -2; d <= 2; d++) tft.drawPixel(cx + dx, yy + d, MINT);
  }
}
void drawEyes(bool closed) {
  tft.fillRect(12, 33, 136, 74, BG);
  if (closed) {
    tft.fillRoundRect(29, 70, 40, 4, 2, MINT);
    tft.fillRoundRect(91, 70, 40, 4, 2, MINT);
  } else {
    drawEye(49, 73);
    drawEye(111, 73);
  }
}
void showDizzy() {
  mode = DIZZY;
  dizzyAt = millis();
  tft.fillScreen(BG);
  const int xs[] = {-15,-12,-4,7,15,16,10,0,-8,-10,-6,1,6,6,2,-1};
  const int ys[] = {1,-10,-16,-15,-8,3,12,15,10,2,-4,-6,-2,2,3,1};
  for (int eye = 0; eye < 2; eye++) {
    int cx = eye ? 111 : 49;
    for (int i = 1; i < 16; i++) {
      tft.drawLine(cx + xs[i-1], 66 + ys[i-1],
                   cx + xs[i], 66 + ys[i], GOLD);
    }
  }
  tft.drawCircle(80, 95, 5, MINT);
}

void eraseFish(int x, int y) {
  int left = max(9, x - 16);
  int right = min(151, x + 17);
  if (right > left) tft.fillRect(left, y - 8, right - left, 17, BG);
}
void paintFish(int x, int y, int dir, uint16_t color) {
  if (x < 23 || x > 137) return;
  tft.fillRoundRect(x - 6, y - 4, 13, 9, 4, color);
  if (dir > 0) {
    tft.fillTriangle(x - 6, y, x - 14, y - 6, x - 14, y + 6, color);
    tft.drawPixel(x + 3, y - 1, BG);
  } else {
    tft.fillTriangle(x + 7, y, x + 15, y - 6, x + 15, y + 6, color);
    tft.drawPixel(x - 3, y - 1, BG);
  }
}
void drawFishBackdrop() {
  tft.fillScreen(BG);     
  drawClockSmall();
  centerText("FISH TANK", 5, 1, MINT, 50, 110);
  tft.drawRoundRect(6, 19, 148, 92, 6, BLUE);
  
  for (int i = 0; i < 6; i++) {
    int x = 15 + i * 24;
    tft.drawLine(x, 107, x + 3, 97, GREEN);
    tft.drawLine(x, 107, x - 3, 98, GREEN);
  }
  tft.drawCircle(26, 28, 2, BLUE);
  tft.drawCircle(101, 28, 2, BLUE);
  tft.drawCircle(140, 26, 1, BLUE);
  centerText("TAP TO SCARE FISH", 117, 1, DIM);
  fishX[0] = 30; fishX[1] = 115; fishX[2] = 74;
  fishDir[0] = 1; fishDir[1] = -1; fishDir[2] = 1;
  fishPhase = FISH_SWIM;
  fishFrameAt = millis();
  for (int i = 0; i < 3; i++) {
    paintFish(fishX[i], fishY[i], fishDir[i],
              i == 0 ? GOLD : i == 1 ? WHITE_C : PURPLE);
  }
}
void scareFish() {
  if (fishPhase != FISH_SWIM) return;
  fishPhase = FISH_SCATTER;
  fishPhaseAt = millis();
  fishHoldUntil = millis() + 7000;
  for (int i = 0; i < 3; i++) fishDir[i] = fishX[i] < 80 ? -1 : 1;
}
// Update only the moving fish.
void updateFish() {
  unsigned long now = millis();
  if (now - fishFrameAt < 110) return; 
  fishFrameAt = now;

  for (int i = 0; i < 3; i++) eraseFish(fishX[i], fishY[i]);

  if (fishPhase == FISH_SWIM) {
    for (int i = 0; i < 3; i++) {
      fishX[i] += fishDir[i] * (i == 1 ? 2 : 1);
      if (fishX[i] >= 137) { fishX[i] = 137; fishDir[i] = -1; }
      if (fishX[i] <= 23)  { fishX[i] = 23;  fishDir[i] = 1; }
    }
  } else if (fishPhase == FISH_SCATTER) {
    for (int i = 0; i < 3; i++) fishX[i] += fishDir[i] * 12;
    if (now - fishPhaseAt >= 1000) {
      fishPhase = FISH_HIDE;
      fishPhaseAt = now;
    }
  } else if (fishPhase == FISH_HIDE) {
    if (now - fishPhaseAt >= 2300) {
      fishX[0] = -20;  fishDir[0] = 1;
      fishX[1] = 180;  fishDir[1] = -1;
      fishX[2] = -20;  fishDir[2] = 1;
      fishPhase = FISH_RETURN;
    }
  } else if (fishPhase == FISH_RETURN) {
    bool arrived = true;
    for (int i = 0; i < 3; i++) {
      int d = fishGoal[i] - fishX[i];
      if (abs(d) > 3) { fishX[i] += d > 0 ? 3 : -3; arrived = false; }
      else fishX[i] = fishGoal[i];
    }
    if (arrived) fishPhase = FISH_SWIM;
  }

  if (fishPhase != FISH_HIDE) {
    for (int i = 0; i < 3; i++) {
      paintFish(fishX[i], fishY[i], fishDir[i],
                i == 0 ? GOLD : i == 1 ? WHITE_C : PURPLE);
    }
  }
}

const char* weatherDescription(int code) {
  if (code == 0) return "CLEAR";
  if (code <= 2) return "PARTLY CLOUDY";
  if (code == 3) return "CLOUDY";
  if (code == 45 || code == 48) return "FOG";
  if (code >= 51 && code <= 57) return "DRIZZLE";
  if (code >= 61 && code <= 67) return "RAIN";
  if (code >= 71 && code <= 77) return "SNOW";
  if (code >= 80 && code <= 82) return "SHOWERS";
  if (code >= 95) return "STORM";
  return "WEATHER";
}
int weatherKind(int code) { 
  if (code <= 1) return 1;
  if (code == 2 || code == 3 || code == 45 || code == 48) return 2;
  return 3;
}

bool getNumber(const String& str, int at, const char* name, float& value) {
  String pattern = String("\"") + name + "\":";
  int p = str.indexOf(pattern, at);
  if (p < 0) return false;
  p += pattern.length();
  while (p < (int)str.length() && str[p] == ' ') p++;
  int end = p;
  while (end < (int)str.length() &&
         ((str[end] >= '0' && str[end] <= '9') ||
          str[end] == '-' || str[end] == '.')) end++;
  if (end == p) return false;
  value = str.substring(p, end).toFloat();
  return true;
}
// Fetch weather for the idle display.
void fetchWeather() {
  weatherAttempted = true;
  weatherLastAttempt = millis();
  if (WiFi.status() != WL_CONNECTED) { weatherOK = false; return; }
  WiFiClientSecure client;
  client.setInsecure(); 
  HTTPClient http;
  http.setTimeout(1800);
  String url =
    "https://api.open-meteo.com/v1/forecast?latitude=" +
    String(WEATHER_LAT, 4) + "&longitude=" + String(WEATHER_LON, 4) +
    "&current=temperature_2m,weather_code&timezone=Europe%2FLondon";
  if (!http.begin(client, url)) { weatherOK = false; return; }
  int status = http.GET();
  if (status == 200) {
    String body = http.getString();
    int pos = body.indexOf("\"current\":");
    float temp = 0, code = 0;
    if (pos >= 0 && getNumber(body, pos, "temperature_2m", temp) &&
        getNumber(body, pos, "weather_code", code)) {
      weatherTemp = temp;
      weatherCode = (int)code;
      weatherOK = true;
      Serial.println("WEATHER READY");
    } else weatherOK = false;
  } else weatherOK = false;
  http.end();
}

void paintCloud(int cx, int cy, uint16_t col) {
  tft.fillRoundRect(cx - 22, cy - 1, 45, 17, 8, col);
  tft.fillCircle(cx - 11, cy - 2, 10, col);
  tft.fillCircle(cx + 6, cy - 6, 14, col);
}
void drawWeatherIcon() {
  tft.fillRect(43, 32, 74, 63, BG); 
  int kind = weatherKind(weatherCode);
  int phase = weatherFrame % 4;

  if (kind == 1) { 
    tft.fillCircle(80, 64, 15, GOLD);
    int radius = 23 + (phase % 2) * 2;
    for (int i = 0; i < 8; i++) {
      float a = i * 0.7853982f;
      int x1 = 80 + (int)(cosf(a) * radius);
      int y1 = 64 + (int)(sinf(a) * radius);
      int x2 = 80 + (int)(cosf(a) * (radius + 6));
      int y2 = 64 + (int)(sinf(a) * (radius + 6));
      tft.drawLine(x1, y1, x2, y2, GOLD);
    }
  } else if (kind == 2) { 
    paintCloud(80 + phase - 2, 62, GREY);
    paintCloud(62 + phase - 2, 68, WHITE_C);
  } else { 
    paintCloud(80, 56, GREY);
    for (int i = 0; i < 4; i++) {
      int x = 60 + i * 13;
      int y = 73 + ((phase * 5 + i * 5) % 12);
      tft.drawLine(x, y, x - 2, y + 5, BLUE);
    }
  }
}
void drawWeatherPage() {
  tft.fillScreen(BG);
  drawClockSmall();
  centerText("LONDON WEATHER", 19, 1, MINT);
  weatherFrame = 0;
  weatherAnimAt = millis();
  drawWeatherIcon();

  float temp = weatherTemp;
  const char* label = weatherDescription(weatherCode);
  char t[20];
  snprintf(t, sizeof(t), "%.0f C", temp);
  centerText(t, 97, 2, WHITE_C);
  centerText(label, 117, 1, GOLD);
}
void updateWeatherIcon() {
  if (millis() - weatherAnimAt < 650) return;
  weatherAnimAt = millis();
  weatherFrame++;
  drawWeatherIcon();
}

void switchIdlePage(IdlePage next) {
  idlePage = next;
  lastIdlePageAt = millis();
  if (next == EYES_PAGE) {
    tft.fillScreen(BG);
    drawEyes(false);
    eyesClosed = false;
    lastBlinkAt = millis();
  } else if (next == FISH_PAGE) {
    drawFishBackdrop();
  } else {
    drawWeatherPage();
  }
}
void showIdle() {
  mode = IDLE;
  lastInteractionAt = millis();
  fishHoldUntil = 0;
  switchIdlePage(EYES_PAGE);
}
void updateIdle() {
  unsigned long now = millis();

  if (idlePage == FISH_PAGE) {
    updateFish();
    if (fishPhase != FISH_SWIM || now < fishHoldUntil) return;
  } else if (idlePage == WEATHER_PAGE) {
    updateWeatherIcon();
  }

  if (now - lastInteractionAt < 10000) {
    if (idlePage != EYES_PAGE) switchIdlePage(EYES_PAGE);
  } else if (now - lastIdlePageAt >= 9500) {
    IdlePage next;
    do {
      next = (IdlePage)random(weatherOK ? 3 : 2);
    } while (next == idlePage);
    switchIdlePage(next);
  }

  if (idlePage == EYES_PAGE) {
    if (!eyesClosed && now - lastBlinkAt >= 4000) {
      drawEyes(true);
      eyesClosed = true;
      eyeCloseAt = now;
    } else if (eyesClosed && now - eyeCloseAt >= 160) {
      drawEyes(false);
      eyesClosed = false;
      lastBlinkAt = now;
    }
  }
}
// Queue one audio event.
void requestAudioTrack(uint8_t trackNumber);

// Queue one audio event.
void requestAudioTrack(uint8_t trackNumber) {
  if (trackNumber < 1 || trackNumber > 3) return;
  queuedAudioTrack = trackNumber;
  audioQueuedAt = millis();
  queuedAudioCommandId = nextAudioCommandId++;
  audioCopiesSent = 0;
  lastAudioSendAt = 0;
  Serial.printf("AUDIO QUEUED: track=%u id=%lu\n", trackNumber,
                (unsigned long)queuedAudioCommandId);
}

// Retry the queued event while the tray is online.
void sendQueuedAudio() {
  if (queuedAudioTrack == 0 || audioCopiesSent >= AUDIO_SEND_COPIES) return;
  if (millis() - audioQueuedAt > AUDIO_COMMAND_TTL_MS) {
    Serial.println("AUDIO DROPPED: event expired before delivery");
    queuedAudioTrack = 0;
    return;
  }
  if (WiFi.status() != WL_CONNECTED || !trayAddressKnown || !trayOnline()) return;
  unsigned long now = millis();
  if (audioCopiesSent > 0 && now - lastAudioSendAt < AUDIO_RETRY_MS) return;

  char message[48];
  snprintf(message, sizeof(message), "MMAUDIO,%u,%lu", queuedAudioTrack,
           (unsigned long)queuedAudioCommandId);
  if (trayUdp.beginPacket(lastTraySenderIP, AUDIO_COMMAND_PORT)) {
    trayUdp.print(message);
    if (trayUdp.endPacket() == 1) {
      audioCopiesSent++;
      lastAudioSendAt = now;
      Serial.printf("AUDIO TX: %s copy=%u\n", message, audioCopiesSent);
      if (audioCopiesSent >= AUDIO_SEND_COPIES) queuedAudioTrack = 0;
    }
  }
}

// Send warning and overdue cues once per task.
void updateTaskAudio() {
  if (mode != TASK_RUNNING) return;
  unsigned long elapsed = (millis() - taskStartedAt) / 1000UL;
  unsigned long estimate = currentTaskBudgetSec;
  if (!warningSoundPlayed && elapsed < estimate && estimate - elapsed <= 30) {
    warningSoundPlayed = true;
    requestAudioTrack(AUDIO_WARNING_TRACK);
  }
  if (!overdueSoundPlayed && elapsed >= estimate) {
    overdueSoundPlayed = true;
    requestAudioTrack(AUDIO_OVERDUE_TRACK);
  }
}

void applyTrayPresence(bool present) {
  bool firstPacket = !trayEverReceived;
  bool changed = firstPacket || (trayPresent != present);
  trayPresent = present;
  trayEverReceived = true;
  lastTrayPacket = millis();

  if (mode == TASK_RUNNING || mode == FINAL_KEY || mode == CONFIRM_UNKNOWN) {
    KeyState old = keyState;
    if (present) {
      keySeenThisSession = true;
      keyState = KEY_IN;
      manualKeyConfirmed = false;
    } else {
      keyState = keySeenThisSession ? KEY_NOT_IN : KEY_UNKNOWN;
    }
    if (keyState != old || mode == CONFIRM_UNKNOWN) keyDirty = true;
  }
  if (changed || millis() - lastTrayDebugAt >= 10000) {
    lastTrayDebugAt = millis();
    Serial.print("TRAY RX: ");
    Serial.print(present ? "ON TRAY" : "ABSENT");
    Serial.print(" | sender seen | online=");
    Serial.println(trayOnline() ? "YES" : "NO");
  }
}
// Read the latest key-tray status.
void receiveTray() {
  if (WiFi.status() != WL_CONNECTED) return;
  int size = trayUdp.parsePacket();
  while (size > 0) {
    char b[64];
    int n = trayUdp.read(b, sizeof(b) - 1);
    if (n > 0) {
      b[n] = '\0';
      
      if (strcmp(b, "MMTRAY6,1") == 0 || strcmp(b, "MMTRAY6,0") == 0) {
        lastTraySenderIP = trayUdp.remoteIP();
        trayAddressKnown = true;
      }
      if (strcmp(b, "MMTRAY6,1") == 0) applyTrayPresence(true);
      else if (strcmp(b, "MMTRAY6,0") == 0) applyTrayPresence(false);
      else if (millis() - lastRawUdpAt > 3000) {
        lastRawUdpAt = millis();
        Serial.print("UDP UNKNOWN PACKET: ");
        Serial.println(b);
      }
    }
    size = trayUdp.parsePacket();
  }
}

// Update the time digits without clearing the screen.
void drawSettingValues() {
  if (mode != SET_HOUR && mode != SET_MINUTE) return;
  char b[12];
  snprintf(b, sizeof(b), "%02d:%02d", targetHour, targetMinute);

  tft.fillRect(4, 53, 152, 57, BG);
  centerText(b, 56, 3, WHITE_C);
  if (mode == SET_HOUR) {
    tft.drawFastHLine(34, 91, 38, MINT);
  } else {
    tft.drawFastHLine(88, 91, 38, MINT);
  }
}
void drawSettingFooter() {
  tft.fillRect(0, 116, 160, 12, BG);
  centerText(realClock() ? "TAP HEAD: START" : "WAIT FOR TIME SYNC",
             117, 1, realClock() ? MINT : GOLD);
}
void showSettingFrame() { 
  tft.fillScreen(BG);
  centerText("DEPART AT", 11, 2, MINT);
  drawSettingValues();
  drawSettingFooter();
}
void enterSetting() {
  mode = SET_HOUR;
  showSettingFrame();
}

void drawSmallKeyIcon() {
  
  tft.drawCircle(131, 46, 7, GOLD);
  tft.drawCircle(131, 46, 6, GOLD);
  tft.drawCircle(131, 46, 3, GOLD);
  tft.drawLine(136, 51, 148, 63, GOLD);
  tft.drawLine(137, 51, 149, 63, GOLD);
  tft.drawLine(143, 58, 148, 53, GOLD);
  tft.drawLine(147, 62, 152, 57, GOLD);
}

void drawKeySide() {
  if (mode != TASK_RUNNING) return;

  String status = trayOnline() ?
      (keyState == KEY_IN ? "IN" :
       keyState == KEY_NOT_IN ? "NOT_IN" : "UNKNOWN")
      : "OFFLINE";

  if (status == cachedKeyState) return;
  cachedKeyState = status;

  tft.fillRect(118, 33, 42, 46, BG);

  if (!trayOnline()) {
    centerText("?", 43, 3, GOLD, 117, 43);
  } else if (keyState == KEY_IN) {
    drawSmallKeyIcon();
  } else if (keyState == KEY_UNKNOWN) {
    centerText("?", 43, 3, DIM, 117, 43);
  }
  
}
void drawTaskRemaining() {
  if (mode != TASK_RUNNING) return;
  long elapsed = (long)((millis() - taskStartedAt) / 1000UL);
  long remain = (long)currentTaskBudgetSec - elapsed;
  unsigned long a = remain < 0 ? (unsigned long)(-remain) : (unsigned long)remain;
  char b[25];
  snprintf(b, sizeof(b), "%s%lu:%02lu",
           remain < 0 ? "OVER +" : "LEFT ", a / 60, a % 60);
  String value(b);
  if (value == cachedTaskTime) return;
  cachedTaskTime = value;
  tft.fillRect(2, 104, 110, 12, BG);
  centerText(b, 106, 1, remain < 0 ? GOLD : MINT, 0, 115);
}
// Show the current task and key status.
void drawTaskScreen() {
  mode = TASK_RUNNING;
  tft.fillScreen(BG);
  cachedTaskTime = cachedKeyState = "";

  char step[22];
  snprintf(step, sizeof(step), "STEP %d/%d", currentStep + 1, TASK_COUNT);
  centerText(step, 5, 1, MINT, 0, 115);
  tft.drawFastHLine(3, 20, 110, DIM);

  const TaskStep& s = TASKS[currentStep];
  centerText(s.line1, 36, strlen(s.line1) <= 5 ? 3 : 2, WHITE_C, 0, 115);
  centerText(s.line2, 63, strlen(s.line2) <= 5 ? 3 : 2, WHITE_C, 0, 115);

  tft.drawFastVLine(115, 19, 109, DIM);

  char est[24];
  snprintf(est, sizeof(est), "EST %lu:%02lu", currentTaskBudgetSec / 60, currentTaskBudgetSec % 60);
  centerText(est, 92, 1, DIM, 0, 115);

  drawKeySide();
  drawTaskRemaining();
  lastTaskTimeUpdate = millis();
}

// Divide the remaining departure time between the remaining tasks.
void allocateCurrentTaskBudget() {
  time_t now = time(nullptr);
  unsigned long available = departureEpoch > now ?
      (unsigned long)(departureEpoch - now) : 0UL;
  unsigned long totalWeight = 0;
  for (int i = currentStep; i < TASK_COUNT; ++i) totalWeight += TASKS[i].weight;
  currentTaskBudgetSec = totalWeight ?
      (unsigned long)(((uint64_t)available * TASKS[currentStep].weight) / totalWeight) : 0UL;
  
  if (available && !currentTaskBudgetSec) currentTaskBudgetSec = 1;
  
if (currentTaskBudgetSec > 45) {
  currentTaskBudgetSec = 45;
}
  Serial.printf("TASK %d: available=%lu s, budget=%lu s\n",
                currentStep + 1, available, currentTaskBudgetSec);
}
void startTasks() {
  if (!realClock()) return;
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  tm.tm_hour = targetHour;
  tm.tm_min = targetMinute;
  tm.tm_sec = 0;
  tm.tm_isdst = -1;
  departureEpoch = mktime(&tm);
  if (departureEpoch <= now) {
    tm.tm_mday++;
    tm.tm_isdst = -1;
    departureEpoch = mktime(&tm);
  }
  currentStep = 0;
  keySeenThisSession = false;
  manualKeyConfirmed = false;
  keyState = KEY_UNKNOWN;
  if (trayOnline() && trayPresent) {
    keySeenThisSession = true;
    keyState = KEY_IN;
  }
  allocateCurrentTaskBudget();
  taskStartedAt = millis();
  warningSoundPlayed = false;
  overdueSoundPlayed = false;
  queuedAudioTrack = 0;
  departureAlerted = false;
  lastLedReminder = millis();
  lastHeadReminder = millis();
  drawTaskScreen();
}
// Show the final key check.
void showFinal() {
  mode = FINAL_KEY;
  tft.fillScreen(BG);
  drawClockSmall();
  centerText("KEY CHECK", 19, 2, MINT);
  if (!trayOnline()) {
    centerText("OFFLINE", 54, 2, GOLD);
    centerText("RECONNECT TRAY", 87, 1, WHITE_C);
  } else if (keyState == KEY_IN) {
    centerText("ON TRAY", 53, 2, GOLD);
    centerText("TAKE YOUR KEYS", 87, 1, WHITE_C);
  } else if (keyState == KEY_NOT_IN) {
    centerText("NOT IN", 54, 2, MINT);
    centerText("TAP HEAD TO LEAVE", 87, 1, WHITE_C);
  } else if (manualKeyConfirmed) {
    centerText("CONFIRMED", 52, 2, MINT);
    centerText("TAP HEAD TO LEAVE", 87, 1, WHITE_C);
  } else {
    centerText("UNKNOWN", 53, 2, GOLD);
    centerText("TAP TO CHECK", 87, 1, WHITE_C);
  }
}
void showUnknownConfirm() {
  mode = CONFIRM_UNKNOWN;
  tft.fillScreen(BG);
  centerText("CONFIRM KEYS", 14, 2, GOLD);
  centerText("DO YOU HAVE", 48, 1, WHITE_C);
  centerText("YOUR KEYS?", 65, 2, WHITE_C);
  centerText("PRESS KNOB: YES", 100, 1, MINT);
}
void showDone() {
  
  requestAudioTrack(AUDIO_COMPLETE_TRACK);
  mode = COMPLETED;
  ledStop();
  tft.fillScreen(BG);
  centerText("ALL DONE!", 29, 2, MINT);
  centerText("LEAVE HOME", 61, 2, WHITE_C);
  tripleFlash();
  swingHead();
}
void nextStep() {
  
  queuedAudioTrack = 0;
  if (currentStep < TASK_COUNT - 1) {
    currentStep++;
    allocateCurrentTaskBudget();
    taskStartedAt = millis();
    warningSoundPlayed = false;
    overdueSoundPlayed = false;
    lastLedReminder = millis();
    lastHeadReminder = millis();
    drawTaskScreen();
  } else {
    showFinal();
  }
  tripleFlash();
  swingHead();
}
void resetRobot() {
  
  queuedAudioTrack = 0;
  warningSoundPlayed = false;
  overdueSoundPlayed = false;
  currentStep = 0;
  taskStartedAt = 0;
  currentTaskBudgetSec = 0;
  departureAlerted = false;
  manualKeyConfirmed = false;
  keySeenThisSession = false;
  keyState = KEY_UNKNOWN;
  ledStop();
  centerHead();
  showIdle();
}

// Handle a head tap according to the current screen.
void onHeadTouch() {
  if (mode == IDLE) {
    if (idlePage == FISH_PAGE) { scareFish(); return; }
    showDizzy();
    tripleFlash();
    swingHead();
  } else if (mode == SET_HOUR || mode == SET_MINUTE) {
    if (realClock()) { startTasks(); tripleFlash(); swingHead(); }
  } else if (mode == TASK_RUNNING) {
    nextStep();
  } else if (mode == FINAL_KEY) {
    if (!trayOnline()) return;
    if (keyState == KEY_NOT_IN ||
        (keyState == KEY_UNKNOWN && manualKeyConfirmed)) showDone();
    else if (keyState == KEY_UNKNOWN) showUnknownConfirm();
  } else if (mode == COMPLETED) {
    resetRobot();
  }
}
void onKnobShort() {

  if (mode == TASK_RUNNING) {
    Serial.println("BUTTON SHORT PRESS: RESET TASK");
    resetRobot();
    return;
  }
  if (mode == SET_HOUR) {
    mode = SET_MINUTE;
    drawSettingValues();      
  } else if (mode == SET_MINUTE) {
    mode = SET_HOUR;
    drawSettingValues();
  } else if (mode == FINAL_KEY && trayOnline() &&
             keyState == KEY_UNKNOWN && !manualKeyConfirmed) {
    showUnknownConfirm();
  } else if (mode == CONFIRM_UNKNOWN) {
    if (trayOnline() && keyState == KEY_UNKNOWN) manualKeyConfirmed = true;
    showFinal();
  } else if (mode == COMPLETED || mode == DIZZY) {
    resetRobot();
  }
}

// Read the encoder button and touch sensor.
void pollInputs() {
  unsigned long now = millis();
  int clk = digitalRead(ENC_CLK);
  if (clk != lastEncoderCLK) {
    if (clk == LOW && now - lastEncoderStep >= 8) {
      int dir = digitalRead(ENC_DT) == HIGH ? 1 : -1;
      if (mode == IDLE || mode == DIZZY) {
        enterSetting();
      } else if (mode == SET_HOUR) {
        targetHour = (targetHour + dir + 24) % 24;
        drawSettingValues();
      } else if (mode == SET_MINUTE) {
        targetMinute = (targetMinute + 5 * dir + 60) % 60;
        drawSettingValues();
      }
      lastEncoderStep = now;
    }
    lastEncoderCLK = clk;
  }

  int raw = digitalRead(ENC_SW);
  if (raw != buttonRaw) { buttonRaw = raw; buttonChangedAt = now; }
  if (raw != buttonStable && now - buttonChangedAt >= 30) {
    buttonStable = raw;
    if (buttonStable == LOW) buttonDownAt = now;
    else if (now - buttonDownAt >= 1800) resetRobot();
    else onKnobShort();
  }

  raw = digitalRead(TOUCH_PIN);
  if (raw != touchRaw) { touchRaw = raw; touchChangedAt = now; }
  if (raw != touchStable && now - touchChangedAt >= 30) {
    touchStable = raw;
    if (touchStable == HIGH && now - lastTouchEvent >= 400) {
      lastTouchEvent = now;
      onHeadTouch();
    }
  }
}

// Handle Wi-Fi connection changes.
void updateWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiStarted) {
      trayUdp.begin(TRAY_PORT);
      configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
      wifiStarted = true;
      Serial.print("ROBOT WIFI IP: ");
      Serial.println(WiFi.localIP());
      Serial.println("UDP TRAY RECEIVER READY");
    }
  } else {
    if (wifiStarted) {
      trayUdp.stop();
      wifiStarted = false;
      trayEverReceived = false;
      trayAddressKnown = false;
      keyDirty = true;
    }
    
    wl_status_t state = WiFi.status();
    if (millis() - wifiRetryAt > 30000 &&
        (state == WL_DISCONNECTED ||
         state == WL_CONNECTION_LOST ||
         state == WL_CONNECT_FAILED ||
         state == WL_NO_SSID_AVAIL)) {
      wifiRetryAt = millis();
      Serial.println("WIFI RECONNECT ATTEMPT");
      WiFi.reconnect();
    }
  }
  if (!clockReported && realClock()) {
    clockReported = true;
    Serial.println("TIME READY");
    if (mode == SET_HOUR || mode == SET_MINUTE) drawSettingFooter();
  }
}

// Update the active screen and timed reminders.
void updateRuntime() {
  static bool wasOnline = false;
  bool online = trayOnline();
  if (online != wasOnline) {
    wasOnline = online;
    keyDirty = true;
    Serial.println(online ? "TRAY ONLINE" : "TRAY OFFLINE");
  }
  if (keyDirty) {
    keyDirty = false;
    if (mode == TASK_RUNNING) drawKeySide();
    else if (mode == FINAL_KEY) showFinal();
    else if (mode == CONFIRM_UNKNOWN &&
             (keyState != KEY_UNKNOWN || !trayOnline())) showFinal();
  }
  if (mode == IDLE) {
    updateIdle();
  } else if (mode == DIZZY && millis() - dizzyAt >= 1800) {
    showIdle();
  } else if (mode == TASK_RUNNING) {
    updateTaskAudio();
    unsigned long now = millis();
    if (now - lastTaskTimeUpdate >= 1000) {
      drawTaskRemaining();  
      lastTaskTimeUpdate = now;
    }
    if (realClock() && time(nullptr) >= departureEpoch && !departureAlerted) {
      departureAlerted = true;
      swingHead();
    }
    if (now - lastLedReminder >= LED_REMINDER_MS) {
      lastLedReminder = now;
      startFlow();
    }
    if (now - lastHeadReminder >= HEAD_REMINDER_MS) {
      lastHeadReminder = now;
      swingHead();
    }
  }
  
  if (mode == IDLE && idlePage == EYES_PAGE &&
      wifiStarted && millis() > 15000 &&
      (!weatherAttempted || millis() - weatherLastAttempt >= 1800000UL)) {
    fetchWeather();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(TOUCH_PIN, INPUT_PULLDOWN);
  lastEncoderCLK = digitalRead(ENC_CLK);
  buttonRaw = buttonStable = digitalRead(ENC_SW);
  touchRaw = touchStable = digitalRead(TOUCH_PIN);

  strip.begin();
  strip.setBrightness(30);
  ledClear();
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.setTextWrap(false);

  headServo.setPeriodHertz(50);
  headServo.attach(SERVO_PIN, 1000, 2000);
  headServo.write(SERVO_CENTER);

  randomSeed(micros());
  nextAudioCommandId = esp_random();  
  showIdle();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_NAME, WIFI_PASS);
  wifiRetryAt = millis();
  Serial.println("THE MISSING MINUTES V13 READY");
}
void loop() {
  updateWiFi();
  receiveTray();
  sendQueuedAudio();  
  pollInputs();
  updateServo();
  updateLed();
  updateRuntime();
  delay(2);
}
