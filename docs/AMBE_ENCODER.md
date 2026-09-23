# AMBE+2 Encoder

TX uses the MIT-licensed `OpenBLIP25/blip25-vocoder` half-rate 3600×2450 encoder.

## Embedded integration

The encoder is built as a Rust static library for ESP32-S3 and exposed to C++ through a small C ABI.

Input:
```text
160 signed PCM samples
8 kHz mono
```

Output:
```text
72 AMBE+2 bits / 9 bytes
```

The Rust output represents the codec code vectors. Before ODTP transmission the firmware applies the DMR-specific 72-bit `rW/rX/rY/rZ` interleave for each 9-byte frame.

Three DMR-ordered 9-byte frames are concatenated into each 27-byte Rewind `0x0920` audio payload.

## ESP32-S3 stack

The Rust encoder requires more stack than the Arduino default loop task. The firmware explicitly raises the loop task stack to 32 KiB and logs the stack high-water mark during testing.

## Patent notice

The upstream project documents that its AMBE+2 implementation may read on active patent claims and identifies US8359197 with an anticipated expiration date of 2028-05-20.

This repository does not make an independent legal conclusion. Builders/distributors should review the upstream patent notice and perform their own due diligence.

Upstream:
https://github.com/OpenBLIP25/blip25-vocoder
