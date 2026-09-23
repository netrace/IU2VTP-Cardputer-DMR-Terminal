# feature/ptt-tx branch override

This branch intentionally develops experimental PTT/TX support. The RX-only rule below remains the production rule for `main`, but on this branch TX work is allowed only through a separate, explicitly gated subsystem. Preserve the known-good RX audio path. Until the verified Rewind TX phase, `sendControl()` must continue blocking DMR voice/header/terminator transmission.

# AGENTS.md

This file contains mandatory project guidance for coding agents and contributors working on **IU2VTP Cardputer DMR Terminal**.

## Project mission

Build and maintain a stable **receive-only DMR-over-IP terminal** for M5Stack Cardputer.

The application:

1. connects to Wi-Fi,
2. connects to a configured DMR/Rewind server,
3. authenticates,
4. subscribes to a talkgroup,
5. receives DMR voice frames,
6. decodes AMBE+2,
7. plays PCM audio,
8. displays live metadata.

## Hard safety / architecture constraint: RX ONLY

This is the most important rule in the repository.

Do not add:

- PTT
- microphone capture
- AMBE encoding
- voice transmission
- voice-header generation for transmission
- voice-terminator generation for transmission
- TX audio buffers
- TX audio queues
- TX DMR packet generation
- any UI that enables DMR voice transmission

Outbound ODTP control-plane packets required for receive operation are allowed:

- KEEPALIVE
- AUTHENTICATION
- SUBSCRIPTION
- CANCELLING
- equivalent minimal control-plane traffic

Do not reinterpret normal control-plane traffic as permission to add an audio transmit path.

## Known-good audio path

Treat the current receive audio pipeline as protected.

Do not modify it unless the task is specifically about audio quality, decoding, timing, or a verified audio bug.

The working pipeline is:

```text
27-byte ODTP DMR audio payload
→ 3 × 9-byte AMBE frames
→ DMR rW/rX/rY/rZ deinterleave
→ classic mbelib AMBE+2 decoder
→ 160 PCM samples/frame
→ 8 kHz mono
→ Cardputer speaker
```

Important characteristics:

- classic mbelib
- optimized cosine path
- DMR-specific deinterleave
- native 8 kHz PCM
- existing buffering strategy
- no encoder

Do not replace the deinterleave mapping with a generic contiguous 72-bit interpretation.

## UI architecture

Navigation is centralized.

Use:

```cpp
navigateTo(...)
navigateBack(...)
resetNavigation(...)
renderCurrentScreen()
```

Do not directly change `uiMode` and then manually clear/draw a different screen unless there is a very specific reason.

When returning to `MAIN`, make sure the RX renderer is invalidated so stale cached screen state cannot survive a transition.

Input screens are transient overlays.

Use the current input-state model:

```cpp
inputReturnMode
inputTarget
inputBuffer
inputTitle
inputNumericOnly
inputSecret
```

When opening input, explicitly preserve the screen that should receive control after confirm/cancel.

Do not infer return destinations from numeric ranges in `inputTarget`.

## Shared screen chrome

Use shared UI helpers:

```cpp
drawAppHeader()
drawFooter(...)
drawScreenBase()
drawScreenChrome(...)
```

Do not duplicate header/footer coordinates or copy footer strings into ad-hoc screen layouts when the shared component can be used.

UI language is **en-US**.

## DMR profiles

Profiles are independent.

The application must support:

- only BrandMeister
- only HamThings
- both
- custom profiles
- deleting an unused default profile

Never assume profile index `0` means the server that should be active.

Always use `activeProfileIndex`.

If the active profile is incomplete but another ready profile exists, normalization may select a valid ready profile.

## Talkgroups

Keep these concepts separate:

- current RX talkgroup
- favorite TG list
- selected favorite TG index

The actual current TG is persisted in NVS using:

```text
currenttg
```

Do not overwrite the current TG from the favorite index on every boot.

Direct TG entry must survive reboot.

## DMR state semantics

Do not conflate:

- Wi-Fi connected
- UDP socket available
- DMR authentication complete
- talkgroup subscription pending
- talkgroup subscription active

`WAIT_SUB_ACK` means the transport/authentication is already established and only the TG subscription confirmation is pending.

Do not show a generic “Connecting to DMR...” message while only switching talkgroups.

## Rewind / ODTP basics

Header:

```text
"REWIND01"
uint16 type LE
uint16 flags LE
uint32 sequence LE
uint16 payload length LE
```

Important packet types:

```text
0x0000 KEEPALIVE
0x0002 CHALLENGE
0x0003 AUTHENTICATION
0x0901 SUBSCRIPTION
0x0902 CANCELLING
0x0911 DMR HEADER FLC
0x0912 DMR TERMINATOR
0x0920 DMR AUDIO
0x0928 SUPERHEADER
```

Authentication:

```text
SHA-256(raw 4-byte challenge + password)
```

Group Voice subscription payload:

```text
uint32 session type = 7
uint32 TG
```

Both little-endian.

## Metadata

Prefer Rewind SUPERHEADER when available.

SUPERHEADER layout:

```text
0..3    session type LE
4..7    source DMR ID LE
8..11   destination DMR ID LE
12..21  source callsign
22..31  target callsign
```

FLC may be used as fallback metadata.

Do not label Rewind flags as DMR timeslots unless independently verified.

## Networking

Never use UDP / lwIP APIs before Wi-Fi is ready.

The firmware previously hit ESP32-S3 lwIP assertions when UDP was initialized without a valid network stack.

Use the existing `ensureUdpStarted()` pattern.

On Wi-Fi loss:

- stop / invalidate UDP state safely
- avoid uncontrolled restart loops
- reconnect deliberately

## Persistence

Settings are stored in Preferences/NVS.

Be careful when adding new keys:

- provide migration behavior
- preserve existing data
- use sensible defaults
- never silently overwrite user credentials

Never print passwords to Serial.

## Secrets

Do not hard-code:

- Wi-Fi credentials
- DMR hotspot passwords
- user-specific secrets

Do not add example real passwords to docs or source.

## Compile hygiene

This project is a single large `main.cpp`, so declaration order matters.

Before finalizing a change:

1. check for duplicate function definitions,
2. check that prototypes appear before first use,
3. check default arguments are declared only once,
4. check navigation functions exist exactly once,
5. check preset initialization still exists,
6. check direct TG entry still works,
7. check RX-only constraints were preserved.

If a helper is used before its definition, add a forward declaration instead of moving unrelated large blocks around.

## Regression checklist

After UI/navigation changes test:

```text
boot
Wi-Fi setup
DMR profile setup
single-profile operation
TG favorite navigation
direct TG entry
direct TG entry a second time
Settings -> back to RX
TG change -> Settings -> back -> direct TG
reboot with non-favorite current TG
server profile switching
subscription ACK
audio playback
caller metadata
```

## Style

Prefer:

- small focused helpers
- explicit state
- centralized navigation
- minimal screen redraws
- stable shared UI components
- short serial logs with clear prefixes

Avoid:

- hidden state transitions
- magic numeric routing logic
- duplicated screen drawing code
- duplicated function definitions
- full-screen redraw loops while receiving audio
- changes to working audio code during unrelated UI work

## Release discipline

When changing behavior:

- bump `APP_VERSION`
- update README if the user-visible behavior changed
- keep archive / folder names versioned
- do not claim a version compiles unless the relevant compile-order checks were actually performed
