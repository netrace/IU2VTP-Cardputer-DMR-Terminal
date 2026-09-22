# IU2VTP Cardputer DMR Terminal

A strictly **receive-only DMR-over-IP terminal** for the **M5Stack Cardputer / StampS3**.

It connects to compatible Open DMR Terminal Protocol / Rewind servers, authenticates, subscribes to a selected talkgroup, receives DMR audio, decodes AMBE+2, and plays the resulting PCM audio through the Cardputer speaker.

The project is designed as a compact portable DMR listening terminal with on-device configuration, multiple DMR server profiles, favorite talkgroups, Wi-Fi setup, live caller metadata, and a small UI optimized for the Cardputer keyboard.

> **Important:** this firmware is intentionally RX-only. It does not implement PTT, microphone capture, DMR voice transmission, voice headers, or voice terminators.

---

## Features

- Strictly receive-only DMR operation
- Open DMR Terminal Protocol / Rewind support
- UDP control-plane authentication
- Group Voice talkgroup subscription
- Multiple DMR server profiles
- Single-profile operation supported
- Favorite talkgroups
- Direct TG entry
- Persistent current TG across reboots
- On-device Wi-Fi scan and setup
- Persistent Wi-Fi configuration
- Adjustable speaker volume
- Live caller / destination metadata
- RadioID.net caller lookup
- Shared UI framework with centralized navigation
- en-US user interface
- M5Stack Cardputer speaker output
- Optimized classic mbelib AMBE+2 decode path

---

## Supported hardware

Current target:

- **M5Stack Cardputer**
- **StampS3 / ESP32-S3**

The project is built with PlatformIO using the Arduino framework.

---

## Build environment

Recommended `platformio.ini`:

```ini
[env:m5stack-stamps3]
platform = espressif32
board = m5stack-stamps3
framework = arduino

monitor_speed = 115200
upload_speed = 1500000

build_flags =
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DARDUINO_USB_MODE=1
```

Build:

```bash
pio run
```

Upload:

```bash
pio run -t upload
```

Firmware output:

```text
.pio/build/m5stack-stamps3/firmware.bin
```

---

## First-time setup

The setup flow is:

```text
Wi-Fi
  ↓
DMR Server
  ↓
Talkgroup
  ↓
RX
```

You may configure only one DMR profile.

For example, HamThings can be the only configured profile while BrandMeister remains unused or is deleted.

---

## Default DMR server templates

The firmware currently ships with templates for:

### BrandMeister

```text
2222.master.brandmeister.network
UDP 54006
```

### HamThings

```text
2221.master.hamthings.it
UDP 54006
```

These are templates only. They are not mandatory.

---

## RX controls

On the main RX screen:

```text
UP / DOWN       Previous / next favorite TG
LEFT / RIGHT    Previous / next ready DMR server
ENTER           Direct TG entry
M               Settings
```

General UI:

```text
ENTER           Open / confirm
ESC             Back / cancel
DEL             Delete character
```

On the Cardputer, directional actions use the existing FN-based key combinations handled by the firmware.

---

## DMR connection states

The UI distinguishes transport/login state from talkgroup subscription state.

Typical states include:

```text
LOGIN
AUTH
AUTH OK
TG...
READY
```

When changing talkgroups, the server connection remains active. The UI may show:

```text
Switching TG...
Waiting for TG confirmation
```

instead of incorrectly reporting that the DMR connection itself is being re-established.

---

## Current TG persistence

The actual RX talkgroup is stored independently of the selected favorite-TG index.

NVS key:

```text
currenttg
```

This means that if you enter a TG directly with `ENTER`, that talkgroup is restored after reboot and used for the next ODTP subscription.

---

## UI architecture

The UI uses a centralized navigation framework.

Core navigation functions:

```cpp
navigateTo(...)
navigateBack(...)
resetNavigation(...)
renderCurrentScreen()
```

Transient text-input state is kept separate from the navigation stack.

Shared visual components include:

```cpp
drawAppHeader()
drawFooter(...)
drawScreenBase()
drawScreenChrome(...)
```

This avoids duplicated header/footer logic across screens and reduces redraw inconsistencies.

---

## DMR protocol notes

The project uses the Rewind / Open DMR Terminal Protocol framing.

Packet header:

```text
"REWIND01"        8 bytes
type              uint16 little-endian
flags             uint16 little-endian
sequence          uint32 little-endian
payload length    uint16 little-endian
```

Relevant packet types include:

```text
0x0000  KEEPALIVE
0x0002  CHALLENGE
0x0003  AUTHENTICATION
0x0901  SUBSCRIPTION
0x0902  CANCELLING
0x0911  DMR HEADER FLC
0x0912  DMR TERMINATOR
0x0920  DMR AUDIO
0x0928  SUPERHEADER
```

Authentication uses:

```text
SHA-256(raw challenge + hotspot password)
```

The expected high-level flow is:

```text
KEEPALIVE
→ CHALLENGE
→ AUTHENTICATION
→ KEEPALIVE ACK
→ SUBSCRIPTION
→ SUBSCRIPTION ACK
→ DMR AUDIO
```

---

## Audio pipeline

The audio path is intentionally conservative because it is already known-good.

Pipeline:

```text
ODTP DMR audio packet
→ 3 × 9-byte AMBE+2 frames
→ DMR deinterleave
→ classic mbelib decode
→ 160 PCM samples/frame
→ 8 kHz mono PCM
→ Cardputer speaker
```

The working DMR deinterleave is based on the `rW/rX/rY/rZ` mapping.

The audio path should be treated as a protected subsystem unless there is a specific audio bug being investigated.

---

## RX-only safety boundary

This project must remain receive-only.

Allowed outbound traffic:

- authentication
- keepalive
- talkgroup subscription
- subscription cancellation
- other minimal ODTP control-plane traffic required to receive audio

Not allowed:

- microphone capture
- PTT
- AMBE encode
- DMR voice packet generation
- DMR voice transmission
- DMR voice headers for transmission
- DMR voice terminators for transmission
- any transmit-mode UI or hardware path

Contributions that add a DMR audio TX path are outside the scope of this project.

---

## Caller metadata

The firmware can use metadata from:

- DMR FLC packets
- Rewind SUPERHEADER packets

SUPERHEADER fields include source and destination IDs plus source/target callsigns.

Optional RadioID.net lookup may provide:

- callsign
- first name
- surname
- city
- state
- country

---

## Project structure

Typical layout:

```text
.
├── platformio.ini
├── README.md
├── AGENTS.md
├── .github/
│   └── copilot-instructions.md
└── src/
    └── main.cpp
```

---

## Development rules

Before changing code, please read:

- [`AGENTS.md`](AGENTS.md)
- [`.github/copilot-instructions.md`](.github/copilot-instructions.md)

These files document the architectural constraints, RX-only boundary, UI conventions, and the known-good audio path.

---

## Current release

Current development baseline:

```text
v1.0.3
```

Key recent work:

- centralized navigation framework
- shared header/footer components
- en-US UI
- single-profile DMR support
- persistent actual RX talkgroup
- improved DMR state display
- transient input-state cleanup

---

## Status

This is an experimental amateur-radio software project under active development.

Test carefully after changes, especially around:

- boot / Wi-Fi reconnection
- profile selection
- talkgroup switching
- navigation transitions
- direct TG entry
- DMR authentication
- subscription ACK handling
- audio playback continuity

---

## Author

**IU2VTP**

Amateur radio software project for M5Stack Cardputer.
