# Wireless protocol

| Direction | Destination port | Payload | Meaning |
|---|---:|---|---|
| ESP8266 -> ESP32 | UDP 4210 | `MMTRAY6,1` | Keys present |
| ESP8266 -> ESP32 | UDP 4210 | `MMTRAY6,0` | Keys absent |
| ESP32 -> ESP8266 | UDP 4211 | `MMAUDIO,<track>,<id>` | Play track 1, 2 or 3 for an event ID |

The tray sends status every 1000 ms, with a state-change path that resets the send timer. It computes the subnet broadcast address from its assigned IP and mask. Optional unicast is enabled by setting a nonzero `robotIP` in the tray source.

The main controller learns the tray address from valid key-state packets. It sends each queued audio event up to three times, separated by at least 90 ms; undeliverable events expire after 10 seconds. The tray suppresses repeats of the most recently handled event ID. There is no playback acknowledgement or guarantee of UDP delivery. The tray accepts senders generally in broadcast-only configuration; when `robotIP` is set, it restricts audio commands to that address.

Key presence observed during a routine is tracked separately from the raw presence reading. This enables the final screen to distinguish keys removed after being seen from a session in which presence has not been established. See `applyTrayPresence`, `startTasks`, `showFinal` and `onHeadTouch` in the main sketch.
