# PTT / TX Development Plan

Branch: `feature/ptt-tx`

Target version: `v1.1.0`

## Goal

Add push-to-talk DMR transmission while preserving the existing stable receive
pipeline.

The current RX path must remain functionally isolated:

```text
ODTP DMR audio
→ DMR deinterleave
→ classic mbelib
→ PCM 8 kHz
→ Cardputer speaker
```

TX is implemented as a separate subsystem.

## Planned TX pipeline

```text
PTT key
→ microphone capture
→ PCM normalization / 8 kHz mono
→ AMBE+2 encoder
→ DMR interleave
→ 3 × 9-byte AMBE frames
→ 27-byte ODTP DMR audio payload
→ Rewind / ODTP TX
```

## Development phases

### ✅ v1.1.0-alpha1 — PTT state machine

No DMR voice packets are transmitted.

- Hold `P` on the RX screen to enter PTT test state.
- Release `P` to return to RX.
- UI shows a red PTT test state and elapsed time.
- PTT is accepted only when the DMR session is ready.
- The existing voice-packet TX guard remains active.
- No microphone capture.
- No AMBE encoder.
- No DMR voice/header/terminator transmission.

Purpose: validate key handling, press/release semantics, UI, and TX state
ownership without transmitting anything.

### ✅ v1.1.0-alpha2 — microphone pipeline

Implemented:
- hold P starts Cardputer microphone capture;
- speaker is stopped while the shared audio peripheral is used by the mic;
- double-buffer capture at 16 kHz mono;
- 320-sample / 20 ms input blocks;
- explicit 2:1 downsample to 160-sample / 8 kHz PCM frames;
- non-blocking PCM queue;
- peak, queue-drop and record-failure counters;
- speaker restored on PTT release;
- still no DMR voice TX.

### 🚧 v1.1.0-alpha3 — AMBE encode validation

Current alpha3 work:

- stable embedded interface: 160 PCM samples at 8 kHz -> 9 AMBE bytes;
- ESP32 backend deliberately reports unavailable for now;
- host reference tool uses MIT-licensed `blip25-vocoder`;
- host CI verifies PCM -> half-rate AMBE+2 code vectors -> FEC decode -> PCM;
- no network voice TX.

The earlier OpenDMR/OP25 candidate is GPL and is therefore not being vendored
into this MIT project.

The next alpha3 step is a Rust/FFI ESP32-S3 backend using the MIT reference
implementation, with the upstream patent notice retained and documented.

### v1.1.0-alpha4 — DMR framing

Add a TX-side DMR interleave/framing module.

Input:

```text
3 × canonical 9-byte AMBE frames
```

Output:

```text
one 27-byte ODTP audio payload
```

Do not reuse RX deinterleave code by reversing assumptions implicitly. Keep
explicit TX mapping and tests.

### v1.1.0-alpha5 — Rewind / ODTP voice TX

Only after the exact server-side sequence is verified:

- call/session start
- group/private destination semantics
- DMR header / SUPERHEADER requirements
- audio subtype / flag sequencing
- 60 ms packet pacing
- terminator
- server ACK/failure behavior
- busy/error handling

## Proposed code boundaries

TX logic should move out of the monolithic UI/network path as it grows.

Suggested interfaces:

```cpp
class TxSession;
class TxAudioCapture;
class AmbeEncoder;
class DmrTxFramer;
class RewindTx;
```

The main loop should only coordinate the TX state machine.

## State model

Initial alpha:

```text
IDLE
  │ hold P
  ▼
PTT_HELD_TEST
  │ release P
  ▼
IDLE
```

Later:

```text
IDLE
→ TX_PREPARE
→ TX_ACTIVE
→ TX_ENDING
→ IDLE
```

Failure paths always return to `IDLE`.

## Safety / regression rules

During early alphas, the existing guard in `sendControl()` must continue to
reject:

- DMR audio
- DMR header FLC
- DMR terminator
- DMR embedded
- SUPERHEADER

The guard is removed or narrowed only in the specific alpha where verified
Rewind TX is intentionally enabled.

Do not modify the known-good RX AMBE decode/deinterleave pipeline as part of
PTT work.

## PTT UX

Alpha1 mapping:

```text
Hold P      PTT test
Release P   return to RX
```

Final key mapping can be revisited after testing ergonomics on the physical
Cardputer.

PTT must be hold-to-talk, not a toggle.

## Before first live transmission

Required checklist:

- encoder licensing resolved
- exact Rewind TX framing verified
- group TG source/destination verified
- packet pacing verified
- header/terminator verified
- maximum continuous TX timeout
- release always terminates TX
- network loss terminates TX
- profile/TG changes blocked while transmitting
- RX audio muted or stopped during TX where necessary
