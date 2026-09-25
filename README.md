# ESP32-CAM intro

ESP-IDF v6 project for the AI-Thinker ESP32-CAM (OV2640, 8MB PSRAM, microSD).

## Modes

Pick one in `idf.py menuconfig` → **ESP32-CAM App → Run mode**.

| Mode | What it does |
|---|---|
| **Stream** (default) | Joins your WiFi (or starts its own `ESP32-CAM` access point), serves a web UI with a live stream, a capture button, flash/torch controls and a list of photos on the SD card. The hardware button also takes photos. |
| **Timelapse** | Deep sleeps between photos. Wakes on a timer (`TIMELAPSE_INTERVAL_SEC`) or the button, saves one 1600×1200 photo and goes back to sleep. |

Photos are saved as `IMG_NNNN.jpg` on the SD card (FAT32). Numbering continues from the files already on the card.

## Build & flash

```sh
source ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py menuconfig          # ESP32-CAM App → WiFi: set your router SSID/password
idf.py -p /dev/cu.usbserial-10 flash monitor
```

## Web UI (stream mode)

- On your router: `http://esp32cam.local/` (or the IP printed on the serial log)
- Fallback access point: join `ESP32-CAM` and open `http://192.168.4.1/`. Unless you set `WIFI_AP_PASSWORD`, each board generates its own random password on first boot. It's printed on the serial log (`net: Access point "ESP32-CAM" up (password: ...)`).

| Endpoint | |
|---|---|
| `GET /` | Web UI |
| `GET :81/stream` | MJPEG live stream (800×600, one viewer at a time) |
| `GET /capture?flash=0\|1` | Take a 1600×1200 photo, save to SD, returns `{"ok":true,"file":"IMG_0012.jpg"}` |
| `GET /torch?level=0-100` | Flash LED as a steady light |
| `GET /photos` | JSON list of photos on the card |
| `GET /photos/IMG_0012.jpg` | Download a photo |

## Wiring

- **Button:** a push button between **GPIO13** and **GND** (internal pull-up is used). It's free because the SD card runs in 1-bit mode, and it can wake the chip from deep sleep. Don't hold it while the board boots, because the SD card samples that line during init.
- The **IO0** button on the ESP32-CAM-MB programmer board can't be used, because GPIO0 drives the camera clock.
- **Flash LED** is GPIO4, driven with PWM (`FLASH_BRIGHTNESS`, default 60%).

## Security

- WiFi credentials live only in `sdkconfig`, which is gitignored. Don't put them in `sdkconfig.defaults`.
- There is no shared default AP password. Each board gets a random one unless you set your own (min 8 chars), and the AP never falls back to an open network.
- **The web UI has no login.** Anyone who can reach the board on the network can watch the stream, take photos and download them from the SD card. On the fallback AP, the WPA2 password is the only protection. On a home/shared network, treat it as visible to every device on that network.
- The stream sends no CORS headers, so other websites open in your browser can't read camera frames.
- Plain HTTP only (no TLS).

## Notes

- If you get brownout resets when WiFi + flash run together, use a better USB cable/port or a 5V supply that can deliver ≥500mA.
- In timelapse mode the camera sensor is powered down and the flash LED pin is held low during deep sleep.
