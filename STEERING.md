# AI Steering Notes

Read `AGENTS.md` first.

Core constraints:

1. Preserve the verified RX mbelib + DMR deinterleave path.
2. Preserve the verified TX path: mic → blip25 → DMR interleave → ODTP.
3. Verified TX call setup is 0x0911 ×2 → 0x0920 audio → 0x0912.
4. G0 / BtnA is primary hold-to-talk; P remains fallback.
5. Group/private TX mode is independent of the active group TG.
6. Keep operation half-duplex.
7. Use centralized UI navigation and shared screen chrome.
8. Keep current TG separate from favorite TG index and private ID.
9. Single-profile operation must work.
10. Never use UDP before Wi-Fi is ready.
11. Preserve separate routine/realtime Rewind sequence spaces.
12. Check compile order, duplicate definitions and regression tests before finishing.
