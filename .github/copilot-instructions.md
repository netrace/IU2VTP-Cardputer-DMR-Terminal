# feature/ptt-tx branch override

On `feature/ptt-tx`, implement PTT/TX only as a separate experimental subsystem. Do not modify the known-good RX deinterleave/mbelib path. Early alphas must keep actual DMR voice TX blocked.

# GitHub Copilot Instructions

You are working on **IU2VTP Cardputer DMR Terminal**, a receive-only DMR-over-IP client for M5Stack Cardputer.

Follow `AGENTS.md` as the authoritative repository guidance.

## Non-negotiable rule

The project must remain **RX-only**.

Never generate code for:

- PTT
- microphone capture
- AMBE encode
- DMR voice/audio TX
- TX voice headers
- TX terminators
- audio transmission queues
- DMR transmission UI

Minimal ODTP control-plane traffic for authentication, keepalive, subscription, and cancellation is allowed.

## Do not casually modify the working audio decoder

The known-good path is:

```text
27-byte DMR audio payload
→ 3 × 9-byte AMBE frames
→ DMR deinterleave
→ classic mbelib
→ 8 kHz mono PCM
→ Cardputer speaker
```

Preserve the existing DMR `rW/rX/rY/rZ` deinterleave mapping unless the task specifically targets decoder correctness.

## UI/navigation rules

Use the shared navigation framework:

```cpp
navigateTo(...)
navigateBack(...)
resetNavigation(...)
renderCurrentScreen()
```

Use the shared visual helpers:

```cpp
drawAppHeader()
drawFooter(...)
drawScreenBase()
drawScreenChrome(...)
```

Do not invent one-off navigation paths.

Do not directly set `uiMode` and clear the display without ensuring the destination screen is rendered.

When returning to `MAIN`, invalidate RX display cache state.

## Input rules

Text/TG input is transient UI state.

Preserve and use:

```cpp
inputReturnMode
inputTarget
inputBuffer
inputTitle
inputNumericOnly
inputSecret
```

Do not derive the return screen from numeric `inputTarget` ranges.

Direct TG entry must remain repeatable and must persist the actual RX TG.

## Profiles

Do not assume profile index 0 is active.

Use:

```cpp
activeProfileIndex
```

Single-profile operation must continue to work.

BrandMeister and HamThings are templates, not mandatory simultaneous configurations.

## Talkgroups

Keep separate:

```text
favoriteTGIndex
favoriteTGs[]
activeTG
```

`activeTG` is the actual RX TG and must survive reboot through the `currenttg` NVS key.

## State semantics

Do not show “Connecting to DMR...” during a TG switch if authentication is already complete.

`WAIT_SUB_ACK` means:

```text
DMR transport/authenticated
+ waiting for TG subscription confirmation
```

## Networking

Do not initialize/use UDP before Wi-Fi is connected.

Use `ensureUdpStarted()`.

Do not reintroduce boot-time lwIP assertions.

## Secrets

Never:

- hard-code Wi-Fi credentials
- hard-code user passwords
- print passwords to Serial
- add real passwords to examples

## C++ compile-order discipline

`main.cpp` is large and forward declarations matter.

Before completing edits, verify:

- one definition per function
- prototypes before use
- no duplicate default arguments
- `makePresetProfiles()` still exists
- navigation functions are not duplicated
- input helper functions are declared before callers

## Language

All user-facing UI and serial/status text should be **en-US**.

## Preferred change strategy

For unrelated UI/network bugs:

- do not touch AMBE/deinterleave/audio code
- do not redesign protocol code
- make the smallest coherent change
- preserve known-good behavior
