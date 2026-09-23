# DM-32 HR-vocoder reverse-engineering notes

This branch is experimenting with two independent AMBE TX backends:

1. the existing portable Rust `blip25-vocoder` backend;
2. a possible firmware-backed backend derived from a **user-supplied**
   `DM32.00.01.046HRVocoder.bin`.

The vendor firmware itself is **not** committed to this repository.

## Confirmed firmware identity

Known DM-32 HR-vocoder 0.46 image:

- file: `DM32.00.01.046HRVocoder.bin`
- size: **890,516 bytes**
- SHA-256:
  `f8484c13c994e920649d7b7bc2b98a98a76cfd0836865e5995a00627fc0c2887`
- wrapper signature: `BFUV32-V2`
- the official Baofeng flasher skips the first **0x100 bytes** before flashing,
  so file offset `f >= 0x100` maps approximately to XIP VMA
  `0x03000000 + (f - 0x100)`.

## HR 0.46 vs normal 0.46

A binary comparison against `DM32.01.01.046.bin` shows two strong stable
alignment regimes:

- a long shared region with HR shifted by about **+128 bytes**;
- later shared code/data shifted by **+19,808 bytes**.

Around the transition, the HR firmware contains an expanded candidate region:

- HR: approximately `0xB3B84 .. 0xBFA5D`
- normal 0.46 corresponding region: approximately `0xB3B04 .. 0xBACFD`
- **net HR growth: 19,680 bytes**

This is the leading candidate for the HR-specific vocoder implementation and
related support code.

## AMBE/DSP signature inside the HR-only expansion

At file offset **0xBF578** (VMA about **0x030BF478**) the HR image contains
CK803 code loading the sequence:

- `0x2AAB`
- `0x147B`
- `0x7FFF`

with the same spacing/style described by independent HR_C7000/CK803S
reverse-engineering as Q15 initialization constants used by the vendor
software AMBE+2 codec.

The important point is that these values are **not sitting in a passive table**:
they appear between CK803 instructions in executable-looking code, and the
location falls squarely inside the HR-specific expanded region.

That is strong evidence that the HR-vocoder payload has been localized, but it
is **not yet enough** to claim the exact encoder entry point or ABI.

## Related independent C7000 research

Independent work on another HR_C7000/CK803S radio has identified a software
AMBE+2 codec with this architecture:

- 8 kHz PCM is processed as two 80-sample halves per 20 ms frame;
- internal representation is a 49-bit MBE parameter vector;
- separate pack/expand helpers serialize the 49-bit params into a 9-byte
  payload;
- codec state/config must be initialized beyond simply calling the low-level
  codec init routine;
- codec code is not position-independent and may rely on fixed RAM/coefficient
  addresses.

That project also found that a firmware-backed codec may require coefficient
tables in the C7000 SAHB SRAM window (`0x18000000...`).

The DM-32 HR image contains many references into `0x18000000..0x18007FFF`,
so a faithful host implementation may need to emulate/copy that address space
rather than execute only one isolated function.

## Why we cannot call this blob directly on ESP32-S3

The DM-32 firmware is CK803/C-SKY machine code. The Cardputer is ESP32-S3
Xtensa. A function pointer into the carved blob therefore cannot execute
natively.

A firmware-backed backend needs one of:

1. a CK803 interpreter/emulator running only the codec code;
2. static/dynamic translation of the required CK803 routines to Xtensa/C++;
3. manual reimplementation of the discovered encoder algorithm.

Full-system QEMU is far too large for a Cardputer firmware, but a small
**codec-only CK803 interpreter** remains plausible if we restrict implemented
instructions and stub the fixed RAM/MMIO that the codec actually touches.

## Current experimental tooling

Run:

```bash
python3 tools/dm32_hrvocoder_probe.py /path/to/DM32.00.01.046HRVocoder.bin
```

It validates the known image/signature and carves the conservative HR-specific
candidate window to:

`build-hrvocoder/hr_candidate.bin`

for offline disassembly/emulator work.

## Next technical step

The next milestone is to identify the call graph around the Q15 initializer and
locate:

- codec/state init;
- PCM analysis/encode entry;
- 49-bit pack routine;
- fixed state and coefficient memory used by those calls.

Once those are known, build a **host-side CK803 harness first**. Only after
PCM→AMBE output can be reproduced on macOS/Linux should a reduced interpreter
be considered for ESP32-S3.

Do not replace the working Rust encoder until this backend produces verified
AMBE frames.
