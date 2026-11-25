// Key tray: pressure sensing and serial audio playback.
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <SoftwareSerial.h>

#include "wifi_secrets.h"

const uint16_t ROBOT_PORT = 4210;
const uint16_t LOCAL_PORT = 4211;

IPAddress robotIP(0, 0, 0, 0);

WiFiUDP udp;

// D5 receives audio TX; D6 sends to audio RX through 1 kOhm.
// This fitted module uses 3.3 V. All modules share ground.
const uint8_t MP3_SOFT_RX = D5;
const uint8_t MP3_SOFT_TX = D6;
SoftwareSerial mp3Serial(MP3_SOFT_RX, MP3_SOFT_TX);

const uint8_t MP3_VOLUME = 10;  
uint32_t lastAudioCommandId = 0;
bool audioCommandSeen = false;
IPAddress broadcastIP(255, 255, 255, 255);

// Keep A0 within the voltage limit of the fitted board.
const int SENSOR_PIN = A0;

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

// Average eight pressure readings.
int readADC() {
  long sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(SENSOR_PIN);
    delay(1);
  }
  return sum / 8;
}

// Apply hysteresis and require a stable sensor change.
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

// Calculate the broadcast address for the current subnet.
IPAddress networkBroadcast() {
  IPAddress ip = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  IPAddress b;
  for (int i = 0; i < 4; i++) {
    b[i] = (ip[i] & mask[i]) | (uint8_t)(~mask[i]);
  }
  return b;
}

// Handle Wi-Fi connection changes.
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

bool sendPacket(IPAddress address) {
  if (!udp.beginPacket(address, ROBOT_PORT)) return false;
  udp.print(keysPresent ? "MMTRAY6,1" : "MMTRAY6,0");
  return udp.endPacket() == 1;
}

// Send the key state to the main robot.
void sendState() {
  if (!wifiUp) return;

  bool broadcastSent = sendPacket(broadcastIP);
  bool unicastSent = robotIP != IPAddress(0, 0, 0, 0)
      ? sendPacket(robotIP) : false;
  Serial.print("TRAY TX: ");
  Serial.print(keysPresent ? "MMTRAY6,1" : "MMTRAY6,0");
  Serial.print(" | broadcast="); Serial.print(broadcastSent);
  Serial.print(" | unicast="); Serial.println(unicastSent);
}

// Write a command directly to the audio module.
void sendMp3Command(uint8_t command, uint16_t parameter) {
  uint8_t packet[10] = {0x7E, 0xFF, 0x06, command, 0x00,
                        (uint8_t)(parameter >> 8), (uint8_t)parameter,
                        0, 0, 0xEF};
  uint16_t sum = 0;
  for (int i = 1; i <= 6; ++i) sum += packet[i];
  uint16_t checksum = (uint16_t)(0 - sum);
  packet[7] = checksum >> 8;
  packet[8] = checksum & 0xFF;
  mp3Serial.write(packet, sizeof(packet));
}

// Set up the audio module and select the microSD card.
void initializeMp3() {
  mp3Serial.begin(9600);
  delay(4000);  
  sendMp3Command(0x09, 2);  
  delay(1000);
  sendMp3Command(0x06, 25);
  delay(200);
  Serial.println("MP3 UART INITIALIZED (playback not yet verified)");
}

// Play a numbered file from the mp3 folder.
void playAudioTrack(uint8_t track) {
  if (track < 1 || track > 3) return;
  sendMp3Command(0x12, track);
  Serial.printf("MP3 COMMAND SENT: /mp3/%04u.mp3\n", (unsigned int)track);
}

void localAudioTest() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c >= '1' && c <= '3') playAudioTrack(c - '0');
    else if (c == '0') { sendMp3Command(0x16, 0); Serial.println("MP3 STOP SENT"); }
    else if (c == '4') { sendMp3Command(0x03, 1); Serial.println("MP3 INDEX 1 SENT"); }
  }
  while (mp3Serial.available()) {
    Serial.printf("MP3 RX: %02X\n", (unsigned int)mp3Serial.read());
  }
}

// Read audio requests and ignore repeated command IDs.
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
  initializeMp3();  
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_NAME, WIFI_PASS);
  lastRetry = millis();
  Serial.println("KEY TRAY V12 READY: 1/2/3 play, 0 stop, 4 indexed track");
}

void loop() {
  updateWiFi();
  localAudioTest();
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
  delay(4);  
}
