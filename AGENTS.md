# AGENTS.md

Mandatory project guidance for **IU2VTP Cardputer DMR Terminal**.

## Project mission

Maintain a stable DMR-over-IP terminal for M5Stack Cardputer with verified **RX and PTT/TX** support over Rewind / Open DMR Terminal Protocol.

## Protected known-good RX path

```text
27-byte ODTP DMR audio
→ 3 × 9-byte AMBE frames
→ DMR rW/rX/rY/rZ deinterleave
→ classic mbelib
→ 8 kHz mono PCM
→ Cardputer speaker
```

Do not replace the DMR deinterleave with a generic contiguous 72-bit interpretation.

## Protected known-good TX path

```text
Cardputer mic 16 kHz
→ 8 kHz / 160 PCM samples
→ blip25 half-rate AMBE+2
→ DMR rW/rX/rY/rZ interleave
→ 3 × 9-byte frames
→ ODTP 0x0920
```

Working call setup:

```text
0x0911 Voice LC
0x0911 Voice LC
0x0920 audio...
0x0912 terminator
```

Do not replace this with a SUPERHEADER-only TX start. `0x0928` is RX metadata, not the verified TX opener.

Voice LC uses:
- FLCO 0x00 group
- FLCO 0x03 private
- 24-bit destination and source IDs
- RS(12,9) parity with Voice-LC mask 0x96

## PTT behavior

- G0 / BtnA is primary hold-to-talk
- P is keyboard fallback
- C switches GROUP / PRIVATE
- I edits private destination DMR ID
- group mode targets `activeTG`
- private mode does not alter `activeTG`
- half-duplex only
- BUSY / FAILURE / network loss abort TX
- maximum TX duration must remain bounded
- profile/TG changes are blocked during active PTT

## Architecture

Keep RX and TX subsystems isolated. Do not casually modify working audio/protocol code while fixing unrelated UI or configuration issues.

## UI

Use centralized navigation:

```cpp
navigateTo(...)
navigateBack(...)
resetNavigation(...)
renderCurrentScreen()
```

Use shared screen helpers:

```cpp
drawAppHeader()
drawFooter(...)
drawScreenBase()
drawScreenChrome(...)
```

UI language is en-US.

## Profiles and talkgroups

Profiles are independent. Use `activeProfileIndex`; never assume profile 0.

Keep separate:
- active/current TG
- favorite TG list
- favorite TG index
- private TX destination

The actual current group TG persists in NVS under `currenttg`.

## Networking

Never use UDP before Wi-Fi is ready. Keep the `ensureUdpStarted()` pattern.

Maintain separate routine and realtime Rewind sequence spaces. Realtime sequence is monotonic for a connection.

## Persistence and secrets

Do not hard-code or print Wi-Fi passwords, hotspot passwords or other user secrets.

## Compile / regression checklist

Before finalizing:
1. compile the ESP32-S3 firmware,
2. run DMR mapping tests,
3. run Rewind TX framing tests,
4. verify boot/Wi-Fi/profile setup,
5. verify RX subscription/audio,
6. verify G0 PTT press/release,
7. verify group TX Last Heard,
8. verify private destination behavior,
9. verify speaker restoration,
10. verify network-loss/BUSY/FAILURE abort behavior.

## Release discipline

When user-visible behavior changes:
- bump `APP_VERSION`,
- update README/docs,
- keep firmware artifact names versioned,
- do not claim live TX validation unless actually tested.
