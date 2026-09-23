# GitHub Copilot Instructions

You are working on **IU2VTP Cardputer DMR Terminal**, an RX/TX DMR-over-IP client for M5Stack Cardputer.

Follow `AGENTS.md` as the authoritative repository guidance.

## Protected RX path

```text
27-byte ODTP audio
→ 3 × 9-byte DMR AMBE frames
→ rW/rX/rY/rZ deinterleave
→ classic mbelib
→ 8 kHz PCM
→ speaker
```

## Protected TX path

```text
microphone
→ 8 kHz / 160 PCM
→ blip25 AMBE+2
→ DMR rW/rX/rY/rZ interleave
→ 3 × 9-byte frames
→ ODTP 0x0920
```

Verified call framing:

```text
0x0911 Voice LC
0x0911 Voice LC
0x0920 audio...
0x0912 terminator
```

Do not change this to SUPERHEADER-only call setup.

## TX semantics

- G0 / BtnA: primary hold-to-talk
- P: fallback hold-to-talk
- C: GROUP / PRIVATE
- I: private destination DMR ID
- group TX destination = active TG
- private TX destination is separate and must not change active TG
- half-duplex only
- abort safely on BUSY, FAILURE, timeout or network loss

## UI/navigation

Use:
```cpp
navigateTo(...)
navigateBack(...)
resetNavigation(...)
renderCurrentScreen()
```

Use shared visual helpers instead of duplicating chrome.

## Profiles / talkgroups

Use `activeProfileIndex`; never assume profile 0.
Keep active TG, favorites and private destination as separate state.

## Networking

Do not initialize/use UDP before Wi-Fi is connected. Preserve `ensureUdpStarted()`.

Routine and realtime Rewind sequence spaces are separate; realtime sequence is monotonic for the connection.

## Secrets

Never hard-code or print user credentials.

## Change strategy

For unrelated UI/config bugs:
- do not touch working RX/TX audio paths,
- do not redesign ODTP framing,
- make the smallest coherent change,
- compile and run relevant host tests.
