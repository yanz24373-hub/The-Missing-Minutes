#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include "wifi_secrets.h"  // Create this local header from wifi_secrets.example.h.

// THE MISSING MINUTES - ESP8266 KEY TRAY FINAL (V11 + MP3 PLAYBACK)
// A0 reads the existing pressure-sensor analog output.
// GPIO14 (D5) receives DFPlayer TX; GPIO12 (D6) sends to DFPlayer RX.
// DFPlayer audio tracks are stored under /mp3/0001.mp3 through 0003.mp3.
// This board continues to transmit the original MMTRAY6 status packets.

const uint16_t ROBOT_PORT = 4210;
const uint16_t LOCAL_PORT = 4211;

// Optional unicast fallback. The original V11 address is not portable.
// Keep 0.0.0.0 to use subnet broadcast only (recommended for changing hotspots).
// If your access point blocks device-to-device broadcast, set the robot IP here.
IPAddress robotIP(0, 0, 0, 0);

WiFiUDP udp;

// NodeMCU labels: D5=GPIO14 (SoftwareSerial RX), D6=GPIO12 (TX).
// Connect DFPlayer TX to D5 through a suitable 3.3 V level shifter/divider.
// Connect D6 through a 1 kOhm resistor to DFPlayer RX.
// Power DFPlayer from regulated 5 V, and share ground with the ESP8266.
// Wire the 8-ohm speaker only between the DFPlayer SPK1 and SPK2 terminals.
const uint8_t MP3_SOFT_RX = D5;
const uint8_t MP3_SOFT_TX = D6;
SoftwareSerial mp3Serial(MP3_SOFT_RX, MP3_SOFT_TX);
DFRobotDFPlayerMini mp3;
bool mp3Ready = false;
const uint8_t MP3_VOLUME = 20;  // Valid range: 0 (mute) to 30 (maximum).
uint32_t lastAudioCommandId = 0;
bool audioCommandSeen = false;
IPAddress broadcastIP(255, 255, 255, 255);

// A0 must receive only the safe analog voltage for your NodeMCU board.
// Do not connect 5 V directly to the ADC pin.
const int SENSOR_PIN = A0;
// Starting thresholds based on earlier tray readings. Adjust after calibration.
// Hysteresis avoids rapidly toggling presence near a single threshold.
const int PRESSURE_ON = 300;
const int PRESSURE_OFF = 150;
const unsigned long STABLE_MS = 700;
const unsigned long SEND_MS = 1000;

bool keysPresent = false;
bool candidate = false;
bool initialized = false;
bool wifiUp = false;
unsigned long candidateAt = 0;
unsigned long lastSend = 0;
unsigned long lastReadPrint = 0;
unsigned long lastRetry = 0;
int adcRaw = 0;

// Average eight samples to reduce analog noise from the pressure sensor.
int readADC() {
  long sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(SENSOR_PIN);
    delay(1);
  }
  return sum / 8;
}

// Use separate entry/exit thresholds and a stability interval.
void updatePressure() {
  adcRaw = readADC();
  bool sensed = keysPresent ?
      (adcRaw > PRESSURE_OFF) : (adcRaw >= PRESSURE_ON);
  if (!initialized) {
    initialized = true;
    keysPresent = sensed;
    candidate = sensed;
    candidateAt = millis();
    return;
  }
  if (sensed != candidate) {
    candidate = sensed;
    candidateAt = millis();
  }
  if (candidate != keysPresent && millis() - candidateAt >= STABLE_MS) {
    keysPresent = candidate;
    Serial.println(keysPresent ? "KEYS CHANGED: ON TRAY" : "KEYS CHANGED: ABSENT");
    lastSend = 0;
  }
}

// Compute a subnet-directed broadcast address from the current Wi-Fi settings.
IPAddress networkBroadcast() {
  IPAddress ip = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  IPAddress b;
  for (int i = 0; i < 4; i++) {
    b[i] = (ip[i] & mask[i]) | (uint8_t)(~mask[i]);
  }
  return b;
}

// Maintain Wi-Fi without blocking the sensor or MP3 command receiver.
void updateWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiUp) {
      wifiUp = true;
      broadcastIP = networkBroadcast();
      udp.begin(LOCAL_PORT);
      Serial.print("TRAY WIFI IP: "); Serial.println(WiFi.localIP());
      Serial.print("BROADCAST: "); Serial.println(broadcastIP);
      Serial.print("UNICAST ROBOT IP: "); Serial.println(robotIP);
      lastSend = 0;
    }
  } else {
    if (wifiUp) { wifiUp = false; udp.stop(); Serial.println("TRAY WIFI LOST"); }
    if (millis() - lastRetry > 30000 &&
        (WiFi.status() == WL_DISCONNECTED ||
         WiFi.status() == WL_CONNECTION_LOST ||
         WiFi.status() == WL_CONNECT_FAILED ||
         WiFi.status() == WL_NO_SSID_AVAIL)) {
      lastRetry = millis();
      Serial.println("TRAY WIFI RECONNECT");
      WiFi.reconnect();
    }
  }
}

// Preserve the exact text protocol expected by the existing ESP32 V11.
bool sendPacket(IPAddress address) {
  if (!udp.beginPacket(address, ROBOT_PORT)) return false;
  udp.print(keysPresent ? "MMTRAY6,1" : "MMTRAY6,0");
  return udp.endPacket() == 1;
}

void sendState() {
  if (!wifiUp) return;
  // Send both: broadcast helps with IP changes, unicast helps
  // on routers/hotspots that don't relay broadcast between clients.
  bool broadcastSent = sendPacket(broadcastIP);
  bool unicastSent = robotIP != IPAddress(0, 0, 0, 0)
      ? sendPacket(robotIP) : false;
  Serial.print("TRAY TX: ");
  Serial.print(keysPresent ? "MMTRAY6,1" : "MMTRAY6,0");
  Serial.print(" | broadcast="); Serial.print(broadcastSent);
  Serial.print(" | unicast="); Serial.println(unicastSent);
}

// ---------------- DFPLAYER MINI ----------------
// The serial parser is disabled for acknowledgements to keep loop timing short.
// Playback requires a compatible DFPlayer Mini, microSD card and speaker.
void initializeMp3() {
  mp3Serial.begin(9600);
  delay(1000);  // Give the MP3 module time to boot and mount the microSD card.
  mp3Ready = mp3.begin(mp3Serial, false, true);
  if (mp3Ready) {
    mp3.volume(MP3_VOLUME);
    Serial.println("MP3 READY");
  } else {
    Serial.println("MP3 NOT DETECTED: sensor and Wi-Fi remain operational");
  }
}

// Play one of three defined audio events from the card's /mp3 directory.
// 1=30-second task warning, 2=task overdue, 3=routine complete.
void playAudioTrack(uint8_t track) {
  if (!mp3Ready) {
    Serial.println("MP3 COMMAND IGNORED: module not ready");
    return;
  }
  if (track < 1 || track > 3) return;
  mp3.playMp3Folder(track);
  Serial.printf("MP3 PLAY: /mp3/%04u.mp3\n", track);
}

// Listen on LOCAL_PORT=4211 for ESP32-originated audio event commands.
// The ESP32 repeats UDP packets; the sequence ID prevents duplicate playback.
void receiveAudioCommands() {
  if (!wifiUp) return;
  int packetSize = udp.parsePacket();
  while (packetSize > 0) {
    char packet[64];
    int size = udp.read(packet, sizeof(packet) - 1);
    if (size > 0) {
      packet[size] = '\0';
      unsigned int track = 0;
      unsigned long commandId = 0;
      if (sscanf(packet, "MMAUDIO,%u,%lu", &track, &commandId) == 2 &&
          track >= 1 && track <= 3 &&
          (!audioCommandSeen || commandId != lastAudioCommandId)) {
        // Ignore unknown senders when the known main-controller IP is configured.
        if (robotIP == IPAddress(0, 0, 0, 0) ||
            udp.remoteIP() == robotIP) {
          audioCommandSeen = true;
          lastAudioCommandId = commandId;
          playAudioTrack((uint8_t)track);
        }
      }
    }
    packetSize = udp.parsePacket();
  }
}

void setup() {
  Serial.begin(115200);
  initializeMp3();  // Initialise sound once before entering the regular loop.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_NAME, WIFI_PASS);
  lastRetry = millis();
  Serial.println("REAL KEY TRAY V11 READY");
}

void loop() {
  updateWiFi();
  receiveAudioCommands();
  updatePressure();
  if (millis() - lastReadPrint >= 800) {
    lastReadPrint = millis();
    Serial.print("ADC="); Serial.print(adcRaw);
    Serial.print(" KEY="); Serial.print(keysPresent ? "ON TRAY" : "ABSENT");
    Serial.print(" WIFI="); Serial.println(wifiUp ? "OK" : "WAIT");
  }
  if (millis() - lastSend >= SEND_MS) {
    lastSend = millis();
    sendState();
  }
  delay(4);  // Yield to the ESP8266 Wi-Fi background tasks.
}
