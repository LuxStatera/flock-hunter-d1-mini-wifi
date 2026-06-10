# Flock Hunter — D1 Mini OLED Edition

A tiny, portable Flock Safety camera detector built on an **ESP8266 D1 Mini** with a **128x64 SH1106 OLED display** and **piezo buzzer**. Passively sniffs WiFi traffic and alerts you when a Flock surveillance camera is nearby.

![Boot screen](images/d1_boot.jpg)

## Credits

This project evolved from the original **[Flock You](https://github.com/colonelpanichacks/flock-you)** project by **[colonelpanichacks](https://github.com/colonelpanichacks)**.

This firmware and README were vibe coded with **[Claude](https://claude.ai)** by Anthropic.

## How It Works

Flock Safety cameras periodically emit WiFi probe requests and other management frames. Researchers identified 32 OUI (MAC address) prefixes consistently associated with Flock camera deployments through field testing. This firmware puts the ESP8266 into promiscuous mode and passively listens on channels 1, 6, and 11 for packets matching those OUI prefixes.

When a match is found, the buzzer chirps, the OLED highlights the detection, and the event is logged to flash storage (LittleFS). Detection history resets on reboot.

### Detection Methods

| Method | Description |
|--------|-------------|
| `wc_prb` | Wildcard probe request — camera searching for networks (highest confidence) |
| `oui_tx` | Transmitter MAC matches a Flock OUI |
| `oui_rx` | Receiver MAC matches a Flock OUI (unicast frame to a Flock device) |

## Hardware

| Part | Pin | Cost |
|------|-----|------|
| ESP8266 D1 Mini | — | ~$3-5 |
| SH1106 128x64 OLED (I2C) | SDA→D2, SCL→D1, VCC→3V3 | ~$2-4 |
| Passive piezo buzzer | (+)→D5, (-)→GND | ~$0.50 |
| **Total** | | **~$6-10** |

### Wiring Diagram

```
D1 Mini          Component
───────          ─────────
3V3         →    OLED VCC
GND         →    OLED GND + Buzzer (-)
D1 (GPIO 5) →    OLED SCL
D2 (GPIO 4) →    OLED SDA
D5 (GPIO 14)→    Buzzer (+)
```

> **Note:** Check the pin labels on your OLED module — some have GND/VCC/SCL/SDA, others have VCC/GND/SCL/SDA. Match the labels, not the position.

## Screen Layout

![Scanning screen](images/d1_scan.jpg)

```
FLK-HUNT  CH:6   C:3     ← header (channel + camera count)
──────────────────────
     Scanning...         ← animated dots
     2 in range          ← active cameras nearby
──────────────────────
3m02  FS:OK BZ  3/100    ← uptime, storage, buzzer, slots used
```

On detection, an **alert screen** shows full camera details (MAC, signal, range, channel, method, hits), followed by a **camera list** showing all detections.

**Range labels** (CLOSE / NEAR / FAR) are rough estimates based on RSSI signal strength. Actual range varies with environment — walls, trees, and antenna orientation all affect the signal.

![Detection alert](images/d1_detect.jpg)

![Detection list](images/d1_list.jpg)

## Audio Alerts

| Event | Sound |
|-------|-------|
| Boot | 6-note descending crow call |
| New detection | Two ascending chirps (2000→2800 Hz) |
| Target in range | Two soft beeps every 10 seconds |

## Building & Flashing

### What You Need
- **Any ESP8266 board** — D1 Mini, NodeMCU, bare ESP-12F, etc.
- **SH1106 128x64 I2C OLED** display (1.3" — not SSD1306)
- **Passive piezo buzzer** (optional but recommended)
- **USB cable** (data cable, not charge-only)
- A computer with Arduino IDE

### Step 1: Install Arduino IDE

Download and install from [arduino.cc/en/software](https://www.arduino.cc/en/software)

### Step 2: Add ESP8266 Board Support

1. Open Arduino IDE
2. Go to **File → Preferences**
3. In "Additional Board Manager URLs", paste:
   ```
   https://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```
4. Go to **Tools → Board → Boards Manager**
5. Search "esp8266" and install **ESP8266 by ESP8266 Community**

### Step 3: Install the U8g2 Library

1. Go to **Sketch → Include Library → Manage Libraries**
2. Search "U8g2"
3. Install **U8g2** by oliver (olikraus)

### Step 4: Load the Sketch

1. Download or clone this repo
2. Open `flock_you_oled.ino` in Arduino IDE
3. The `types.h` file must be in the same folder — Arduino IDE will show it as a second tab

### Step 5: Configure Board Settings

Select **Tools → Board → `Generic ESP8266 Module`** and configure:

| Setting | Value |
|---------|-------|
| **Board** | `Generic ESP8266 Module` |
| **Flash Size** | `4MB (FS:1MB OTA:~1019KB)` |
| **Upload Speed** | `921600` |
| **Port** | Select the port that appears when you plug in your board |

> **Using a D1 Mini?** You can also select `LOLIN(WEMOS) D1 R2 & mini` which pre-configures the flash size for you.

> **Any ESP8266 board works.** The firmware uses raw GPIO numbers — not board-specific pin labels like D1/D2. Just wire the OLED and buzzer to the correct GPIOs:
> - **GPIO 4** → OLED SDA
> - **GPIO 5** → OLED SCL
> - **GPIO 14** → Buzzer (+)

> **No port showing up?** Install the [CH340 driver](https://sparks.gogo.co.nz/ch340.html) (D1 Mini / NodeMCU) or [CP2102 driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) depending on your board's USB chip.

### Display Compatibility

This firmware is written for the **SH1106** OLED controller (common on 1.3" modules). If you have the smaller **SSD1306** (0.96" OLED), find and change this line in the sketch from:
```
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(...)
```
to:
```
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(...)
```

### Alternative Boards

**HW-634A Generic ESP8266:** This board uses different default I2C pins. Change the pin defines near the top of the sketch:
```
#define OLED_SDA 14
#define OLED_SCL 12
```
Note: GPIO 14 is the default buzzer pin, so if you're using a buzzer with the HW-634A, change `BUZZER_PIN` to a free GPIO as well.

### Step 6: Upload

Click the **Upload** button (right arrow) in Arduino IDE. Once complete, the device will reboot, play the startup sound, and begin scanning.

## Serial Output

At 115200 baud, outputs JSON for each detection:
```json
{"event":"detection","method":"wifi_oui_tx","mac":"70:c9:4e:xx:xx:xx","oui":"70:c9:4e","rssi":-62,"ch":6}
```

Heartbeat status every 30 seconds:
```
[fy] ch=6 det=3 heap=38240
```

## Features

- **Passive sniffing** — no transmitting, no network connections
- **32 OUI signatures** — matches known Flock Safety hardware prefixes
- **Channel hopping** — cycles channels 1, 6, 11 (150ms dwell)
- **Session logging** — detections saved to LittleFS during each session (resets on reboot)
- **Deduplication** — 5-second cooldown per MAC, 30-second rediscovery window
- **Heartbeat beeps** — soft reminder when a detected camera is still in range
- **100 detection slots** — optimized for ESP8266's limited RAM (~80KB)

## 32 Flock Safety OUI Prefixes

```
70:c9:4e  3c:91:80  d8:f3:bc  80:30:49  b8:35:32
14:5a:fc  74:4c:a1  08:3a:88  9c:2f:9d  c0:35:32
94:08:53  e4:aa:ea  f4:6a:dd  f8:a2:d6  24:b2:b9
00:f4:8d  d0:39:57  e8:d0:fc  e0:4f:43  b8:1e:a4
70:08:94  58:8e:81  ec:1b:bd  3c:71:bf  58:00:e3
90:35:ea  5c:93:a2  64:6e:69  48:27:ea  a4:cf:12
82:6b:f2  b4:1e:52
```

## Flock Hunter Family

- **[Flock Hunter CYD WiFi](https://github.com/LuxStatera/flock-hunter-cyd-wifi)** — WiFi detector with 32 OUI prefixes + PCAP capture
- **[Flock Hunter CYD BLE](https://github.com/LuxStatera/flock-hunter-cyd-ble)** — Bluetooth detector scanning for manufacturer ID 0x09C8
- **[Flock Hunter D1 Mini WiFi](https://github.com/LuxStatera/flock-hunter-d1-mini-wifi)** — Compact WiFi detector with OLED display + piezo buzzer (this project)
- **[Flock Camera Emulator](https://github.com/LuxStatera/flock-camera-emulator)** — ESP32 test tool for validating detectors

## Legal Disclaimer

This device is a **passive receiver only**. It does not transmit, deauthenticate, jam, or interfere with any wireless communications. Monitoring publicly broadcast WiFi management frames is generally legal, but laws vary by jurisdiction. Check your local laws before use. This project is for educational and research purposes.
