# PTT / TX Implementation Notes

Target release: **v1.1.0**

## Status

Live PTT/TX is implemented and validated against Open DMR Terminal servers.

Verified:
- physical G0 / BtnA hold-to-talk
- keyboard P fallback
- Cardputer microphone capture
- 16 kHz → 8 kHz downsample
- embedded blip25 AMBE+2 encoder
- DMR 72-bit interleave
- 27-byte ODTP voice payloads
- network pacing
- group TX
- private TX
- Voice-LC call setup
- terminator handling
- Last Heard visibility
- live voice audio
- half-duplex speaker/microphone switching

## TX pipeline

```text
PTT
→ mic 16 kHz
→ 8 kHz / 160 PCM samples
→ AMBE+2 encode
→ DMR rW/rX/rY/rZ interleave
→ 3 × 9-byte AMBE frames
→ 27-byte ODTP 0x0920 payload
→ UDP
```

## Verified call sequence

Reverse engineering and live testing established the working ODTP transmit sequence:

```text
0x0911 Voice LC
0x0911 Voice LC
0x0920 audio
0x0920 audio
...
0x0912 terminator
```

A SUPERHEADER-only call opener was rejected by live servers.

Voice LC is 12 bytes:
- FLCO 0x00 for group voice
- FLCO 0x03 for private voice
- FID 0
- service options 0
- destination DMR ID, 24-bit big-endian
- source DMR ID, 24-bit big-endian
- RS(12,9) parity masked with 0x96

## Controls

```text
G0 / BtnA   Hold-to-talk
P           Keyboard fallback PTT
C           GROUP / PRIVATE
I           Private DMR ID
```

Group/private mode switching is independent from the current group TG.

## Runtime protection

- PTT requires a ready DMR session
- half-duplex RX suppression during TX
- BUSY / FAILURE abort
- Wi-Fi/network-loss abort
- max TX duration
- re-key latch after forced abort
- TG/server changes blocked during TX
- speaker restored after microphone shutdown

## Encoder

The embedded backend uses `OpenBLIP25/blip25-vocoder` through the Rust/C ABI bridge in `codec/blip25_ffi`.

See `docs/AMBE_ENCODER.md`.
