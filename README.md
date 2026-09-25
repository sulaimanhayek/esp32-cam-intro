# 📷 ESP32-CAM Intro

Camera firmware for the AI-Thinker ESP32-CAM, built on ESP-IDF v6. It streams live video to a password-protected web UI and saves full-resolution photos to microSD. A deep-sleep timelapse mode is also included.

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0-E7352C) ![Target](https://img.shields.io/badge/target-ESP32-blue) ![Sensor](https://img.shields.io/badge/sensor-OV2640-lightgrey)

---

## ✨ Features

| | |
|---|---|
| 🎥 **Live stream** | MJPEG at 800×600 in the browser |
| 📸 **Photo capture** | 1600×1200 JPEG saved to microSD from the web UI or a hardware button |
| 💡 **Flash** | On/off switch for the onboard LED; photos taken while it is on are lit |
| 🔐 **Authentication** | Password login, session cookies and brute-force lockout |
| 📶 **WiFi** | Joins your network, or starts its own WPA2 access point |
| ⏱️ **Timelapse** | Wakes on a timer or button press, takes one photo, then returns to deep sleep |
| 🗂️ **Photo browser** | Lists the photos on the card and downloads them from the web UI |

---

## 🧰 Hardware

| Component | Notes |
|---|---|
| AI-Thinker ESP32-CAM | ESP32 with 4 MB flash, 8 MB PSRAM and an OV2640 sensor |
| microSD card | FAT32, 32 GB or smaller |
| ESP32-CAM-MB or a USB-UART adapter | For flashing and the serial log |
| Push button *(optional)* | Between **GPIO13** and **GND** |

### 🔌 Pin usage

| GPIO | Function |
|---|---|
| 2, 14, 15 | microSD (1-bit SDMMC mode) |
| 4 | Flash LED (PWM) |
| 13 | Capture button, active low, internal pull-up, deep-sleep wake source |
| 0 | Camera clock (the `IO0` button on the MB board can't be used) |

> ⚠️ Don't hold the GPIO13 button during boot. The SD card driver samples that line while it starts up.

---

## 🚀 Getting started

**1. Load ESP-IDF**
```sh
source ~/.espressif/tools/activate_idf_v6.0.2.sh
```

**2. Configure** *(optional)*
```sh
idf.py menuconfig    # ESP32-CAM App
```

**3. Build, flash and monitor**
```sh
idf.py -p /dev/cu.usbserial-10 flash monitor
```

**4. Connect**

| Network | URL |
|---|---|
| Your WiFi (if configured) | `http://esp32cam.local/` or the IP on the serial log |
| Fallback access point `ESP32-CAM` | `http://192.168.4.1/` |

On first boot the serial log prints the generated credentials:

```
net: Access point "ESP32-CAM" up (password: ...)
auth: Web UI password: ...
```

---

## ⚙️ Configuration

All options are under `idf.py menuconfig` → **ESP32-CAM App**.

| Option | Default | Description |
|---|---|---|
| Run mode | Stream | `Stream` or `Timelapse` |
| `WIFI_SSID` / `WIFI_PASSWORD` | *(empty)* | Your network. Leave empty to always use the access point |
| `WIFI_AP_SSID` | `ESP32-CAM` | Name of the fallback access point |
| `WIFI_AP_PASSWORD` | *(random)* | Access point password, min 8 characters |
| `WEB_PASSWORD` | *(random)* | Web UI password, min 8 characters |
| `WIFI_HOSTNAME` | `esp32cam` | mDNS hostname |
| `BUTTON_GPIO` | `13` | Capture button pin |
| `FLASH_BRIGHTNESS` | `60` | Flash LED brightness (%) |
| `PHOTO_USE_FLASH` | `n` | Fire the flash for button and timelapse photos |
| `TIMELAPSE_INTERVAL_SEC` | `60` | Seconds between timelapse photos |

When a password is left empty, the board generates a random one on first boot and stores it in NVS. It stays the same across reboots. `idf.py erase-flash` generates a new one.

---

## 🎛️ Modes

### 🎥 Stream (default)
- Starts the camera at 800×600, mounts the SD card and connects to WiFi, falling back to its own access point.
- Serves the web UI on port 80 and the stream on port 81.
- Takes a photo on each button press.

### ⏱️ Timelapse
- Wakes on a timer (`TIMELAPSE_INTERVAL_SEC`) or a button press and takes one 1600×1200 photo.
- Powers down the camera, holds the flash LED low and returns to deep sleep.
- Keeps photo numbering in RTC memory, so the card isn't rescanned on every wake.

Photos are saved as `IMG_NNNN.jpg`, and numbering continues from the files already on the card.

---

## 🌐 HTTP API

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/` | Web UI, or the login page without a session |
| `POST` | `/login` | Form field `password`. Sets the session cookie |
| `POST` | `/logout` | Ends the session |
| `GET` | `:81/stream` | MJPEG stream, one viewer at a time |
| `POST` | `/capture?flash=0\|1` | Takes and saves a photo → `{"ok":true,"file":"IMG_0012.jpg"}` |
| `GET` | `/torch` | Flash state → `{"on":false}` |
| `POST` | `/torch?on=0\|1` | Turns the flash LED on or off |
| `GET` | `/photos` | Photo list → `{"sd":true,"photos":[{"name":"IMG_0012.jpg","size":187342}]}` |
| `GET` | `/photos/IMG_NNNN.jpg` | Downloads a photo |

Every endpoint except `/` and `/login` returns `401` without a valid session.

---

## 🔐 Security

| Area | Implementation |
|---|---|
| 🔑 Credentials | No shared defaults. WiFi and web passwords are random per device unless you set them. The access point is always WPA2 |
| 🍪 Sessions | 128-bit tokens from the hardware RNG, held in RAM only. 12 h lifetime, max 4 at a time. The cookie is `HttpOnly` and `SameSite=Strict` |
| 🛑 Brute force | 5 failed logins lock login for 30 s. The lockout doubles on each further failure, up to 15 min. Passwords are compared in constant time |
| 🧱 Hardening | Changes are POST-only (CSRF-safe with `SameSite`), `X-Frame-Options: DENY`, `nosniff`, `no-store`, no CORS, and downloads are limited to `IMG_*` files in the card root |
| 🔒 Secrets in git | WiFi credentials live only in `sdkconfig`, which is gitignored. Never put them in `sdkconfig.defaults` |

> ⚠️ **Plain HTTP (no TLS).** The password and session cookie are not encrypted in transit. Use the device only on networks you trust. The access point's WPA2 encrypts the wireless link.

> ⚠️ The login lockout applies to the whole device. Someone guessing passwords can also temporarily lock out legitimate users.

---

## 🛠️ Troubleshooting

| Symptom | Fix |
|---|---|
| `SD mount failed: ESP_ERR_TIMEOUT` | Reseat the card and check it is FAT32. It mounts on the next capture without a reboot |
| Brownout resets | Use a better USB cable or port, or a 5 V supply rated for at least 500 mA |
| `Could not exclusively lock port` | Another serial monitor has the port open. Close it first |
| Stream stops when a second viewer opens it | Only one stream client is supported at a time |

---

## 📁 Project layout

```
main/
├── main.c           # Mode selection, button task, deep sleep
├── camera.c         # OV2640 setup and photo capture
├── storage.c        # microSD mount and JPEG saving
├── flash_led.c      # Flash LED PWM
├── net.c            # WiFi station / access point and mDNS
├── web.c            # HTTP server, API and MJPEG stream
├── auth.c           # Login, sessions and lockout
├── secret.c         # Per-device random secrets in NVS
├── index.html       # Web UI
└── login.html       # Login page
```
