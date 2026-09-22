# AI Steering Notes

Read `AGENTS.md` before making changes.

Core constraints:

1. RX-only DMR terminal.
2. No voice/audio TX path.
3. Preserve known-good mbelib + DMR deinterleave audio pipeline.
4. Use centralized UI navigation.
5. Use shared header/footer components.
6. en-US UI only.
7. Keep current TG separate from favorite TG index.
8. Single-profile DMR configuration must work.
9. Do not use UDP before Wi-Fi is ready.
10. Check for duplicate definitions and missing forward declarations before finishing.
