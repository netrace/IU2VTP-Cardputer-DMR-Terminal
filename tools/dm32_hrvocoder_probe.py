#!/usr/bin/env python3
"""
Probe/carve the Baofeng DM-32 HR-vocoder firmware used for reverse engineering.

This tool does NOT redistribute the firmware. Point it at a locally-owned
DM32.00.01.046HRVocoder.bin.

Known public SHA-256:
  f8484c13c994e920649d7b7bc2b98a98a76cfd0836865e5995a00627fc0c2887

Findings for 0.46 HR vs normal 0.46:
  - official Baofeng 0x100-byte wrapper/header
  - XIP firmware begins after that header
  - HR-specific expanded region is approximately 0xB3B84..0xBFA5D
  - corresponding normal-firmware region is ~0xB3B04..0xBACFD
  - net growth: 19,680 bytes
  - Q15 AMBE-like init signature 0x2AAB,0x147B,0x7FFF occurs in CK803 code
    at HR file offset 0xBF578, inside the expanded HR-only region.

The carve below is intentionally conservative: it extracts the whole
HR-specific candidate window for offline analysis/emulation.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

EXPECTED_SHA256 = "f8484c13c994e920649d7b7bc2b98a98a76cfd0836865e5995a00627fc0c2887"
HEADER_SIZE = 0x100
HR_CANDIDATE_START = 0xB3B84
HR_CANDIDATE_END = 0xBFA5D
Q15_SIGNATURE_OFFSET = 0xBF578
XIP_BASE = 0x03000000


def fileoff_to_vma(off: int) -> int:
    if off < HEADER_SIZE:
        raise ValueError("offset is inside the Baofeng wrapper header")
    return XIP_BASE + (off - HEADER_SIZE)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware", type=Path)
    ap.add_argument("--out", type=Path, default=Path("build-hrvocoder/hr_candidate.bin"))
    args = ap.parse_args()

    data = args.firmware.read_bytes()
    digest = hashlib.sha256(data).hexdigest()

    print(f"size       : {len(data)}")
    print(f"sha256     : {digest}")
    print(f"header     : {data[:9]!r}")

    if data[:9] != b"BFUV32-V2":
        raise SystemExit("not a BFUV32-V2 firmware image")

    if digest != EXPECTED_SHA256:
        print("WARNING: SHA-256 differs from the known 0.46 HR image")

    sig = struct.unpack_from("<HH", data, Q15_SIGNATURE_OFFSET)
    sat = struct.unpack_from("<H", data, Q15_SIGNATURE_OFFSET + 0x18)[0]
    print(
        "Q15 probe  : "
        f"0x{sig[0]:04X}, 0x{sig[1]:04X}, ... 0x{sat:04X} "
        f"@ file 0x{Q15_SIGNATURE_OFFSET:X} / VMA 0x{fileoff_to_vma(Q15_SIGNATURE_OFFSET):08X}"
    )

    # Exact instruction spacing in this firmware puts 0x2AAB at +0x00,
    # 0x147B at +0x08 and 0x7FFF at +0x18.
    if not (
        struct.unpack_from("<H", data, Q15_SIGNATURE_OFFSET)[0] == 0x2AAB
        and struct.unpack_from("<H", data, Q15_SIGNATURE_OFFSET + 0x08)[0] == 0x147B
        and struct.unpack_from("<H", data, Q15_SIGNATURE_OFFSET + 0x18)[0] == 0x7FFF
    ):
        raise SystemExit("expected Q15 signature not found")

    blob = data[HR_CANDIDATE_START:HR_CANDIDATE_END]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)

    print(
        f"candidate  : file 0x{HR_CANDIDATE_START:X}..0x{HR_CANDIDATE_END:X} "
        f"({len(blob)} bytes)"
    )
    print(
        f"VMA range  : 0x{fileoff_to_vma(HR_CANDIDATE_START):08X}.."
        f"0x{fileoff_to_vma(HR_CANDIDATE_END):08X}"
    )
    print(f"wrote      : {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
