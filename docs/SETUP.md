# Setup and operation

## Prepare the Arduino environment

1. Install the ESP32 Arduino board core (Espressif) and ESP8266 Arduino board core. Use board definitions matching the physical boards. The tray uses NodeMCU D5/D6 aliases, so select the appropriate NodeMCU-compatible definition or adapt those labels for your board.
2. Install Adafruit GFX Library, Adafruit ST7735 and ST7789 Library, Adafruit NeoPixel and ESP32Servo for the main controller. Install DFRobotDFPlayerMini for the tray and ensure ESP8266 SoftwareSerial is available. Install any dependencies requested by Library Manager.
3. In **each** sketch directory, copy `wifi_secrets.example.h` to `wifi_secrets.h` and fill in the same 2.4 GHz Wi-Fi SSID and password. The real header is ignored by Git. Do not publish it.
4. Open `firmware/ESP32_Main/ESP32_Main.ino` and `firmware/ESP8266_Tray/ESP8266_Tray.ino` separately. Select the matching board/port, verify, and upload each sketch to its controller.
5. Open Serial Monitor at 115200 baud. The main controller should report Wi-Fi, time readiness and tray packets. The tray reports ADC values, Wi-Fi and MP3 readiness.

Exact core/library versions were not supplied and no compilation is claimed for this package. Record versions when validating a reproduction. Follow [connections](../hardware/CONNECTIONS.md) before applying power.

## Prepare the audio card

Place your own compatible audio files on the DFPlayer microSD card:

| File | Event |
|---|---|
| `/mp3/0001.mp3` | 30 seconds before the current task estimate expires |
| `/mp3/0002.mp3` | Current task reaches its estimated duration |
| `/mp3/0003.mp3` | Final key check accepted and routine completed |

Audio files are not included. The tray uses 9600-baud serial and volume 20 (range 0-30). If MP3 initialization fails, the pressure sensor and Wi-Fi loop continue.

## Network and time

Both devices must be on the same Wi-Fi subnet with client-to-client UDP allowed. The tray sends subnet broadcasts; leave `robotIP(0,0,0,0)` for broadcast-only operation. If the router blocks broadcast, set `robotIP` in the tray sketch to the main controller's current IP. The main controller learns the tray IP from incoming status packets.

UDP destination ports are 4210 (main robot status receiver) and 4211 (tray audio receiver). A tray is considered offline after 7 seconds without a valid packet. Internet access is needed for NTP time synchronization and the optional weather page. Starting a routine requires a synchronized clock. Time is configured for UK GMT/BST; weather defaults are latitude 51.55 and longitude -0.22. Adjust these source constants for a different deployment.

The weather HTTPS client uses `setInsecure()` in the supplied prototype. The local UDP protocol has no application authentication. Use an appropriate trusted prototype network; do not treat the protocol as a secure service.

## Run a routine

1. Rotate the knob from idle to enter departure-time settings. Rotate to change the selected hour/minute; short-press the knob to switch fields. Minutes change in five-minute increments.
2. When time is ready, touch the head to start. A time already passed is interpreted as the next day.
3. Complete GET DRESSED (3 minutes), PACK BAG (2 minutes) and GET READY (2 minutes), touching the head to advance. These are guidance estimates, not enforced completion times.
4. The robot gives light reminders every 10 seconds and hat-movement reminders every 20 seconds while tasks are active. Audio warning and overdue events each occur at most once per task.
5. At the final key check, keys on the tray must be removed. An online tray reporting absent keys permits a head touch to finish. If the online state is unknown, confirm possession using the knob and then touch the head. An offline tray blocks completion until communication returns.
6. Short-pressing the knob during a running task resets to idle. A long press of at least 1.8 seconds also resets. Touching the head after completion resets the robot.

Serial characters `1`, `2`, `3` preview sunny/cloudy/rainy weather; `0` exits the preview. These previews do not modify fetched weather data.

## Checks on the physical build

Confirm display orientation, encoder direction, touch polarity, safe servo travel, all LEDs, ADC readings with/without keys, UDP reception, all three audio tracks, and the final-key offline/unknown paths. Calibrate pressure thresholds against the actual tray. These are reproduction checks to perform on hardware; they have not been run as part of this documentation task.
